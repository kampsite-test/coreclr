// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tools.RuntimeClient;

namespace Tracing.Tests.Common
{
    public class Logger
    {
        public static Logger logger = new Logger();
        private TextWriter _log;
        private Stopwatch _sw;
        public Logger(TextWriter log = null)
        {
            _log = log ?? Console.Out;
            _sw = new Stopwatch();
        }

        public void Log(string message)
        {
            if (!_sw.IsRunning)
                _sw.Start();
            _log.WriteLine($"{_sw.Elapsed.TotalSeconds,5:f1}s: {message}");
        }
    }

    public class ExpectedEventCount
    {
        // The acceptable percent error on the expected value
        // represented as a floating point value in [0,1].
        public float Error { get; private set; }

        // The expected count of events. A value of -1 indicates
        // that count does not matter, and we are simply testing
        // that the provider exists in the trace.
        public int Count { get; private set; }

        public ExpectedEventCount(int count, float error = 0.0f)
        {
            Count = count;
            Error = error;
        }

        public bool Validate(int actualValue)
        {
            return Count == -1 || CheckErrorBounds(actualValue);
        }

        public bool CheckErrorBounds(int actualValue)
        {
            return Math.Abs(actualValue - Count) <= (Count * Error);
        }

        public static implicit operator ExpectedEventCount(int i)
        {
            return new ExpectedEventCount(i);
        }

        public override string ToString()
        {
            return $"{Count} +- {Count * Error}";
        }
    }

    // This event source is used by the test infra to
    // to insure that providers have finished being enabled
    // for the session being observed. Since the client API
    // returns the pipe for reading _before_ it finishes
    // enabling the providers to write to that session,
    // we need to guarantee that our providers are on before
    // sending events. This is a _unique_ problem I imagine
    // should _only_ affect scenarios like these tests
    // where the reading and sending of events are required
    // to synchronize.
    public sealed class SentinelEventSource : EventSource
    {
        private SentinelEventSource() {}
        public static SentinelEventSource Log = new SentinelEventSource();
        public void SentinelEvent() { WriteEvent(1, "SentinelEvent"); }
    }

    public static class SessionConfigurationExtensions
    {
        public static SessionConfiguration InjectSentinel(this SessionConfiguration sessionConfiguration)
        {
            var newProviderList = new List<Provider>(sessionConfiguration.Providers);
            newProviderList.Add(new Provider("SentinelEventSource"));
            return new SessionConfiguration(sessionConfiguration.CircularBufferSizeInMB, sessionConfiguration.Format, newProviderList.AsReadOnly());
        }
    }

    public class IpcTraceTest
    {
        // This Action is executed while the trace is being collected.
        private Action _eventGeneratingAction;

        // A dictionary of event providers to number of events.
        // A count of -1 indicates that you are only testing for the presence of the provider
        // and don't care about the number of events sent
        private Dictionary<string, ExpectedEventCount> _expectedEventCounts;
        private Dictionary<string, int> _actualEventCounts = new Dictionary<string, int>();
        private int _droppedEvents = 0;
        private SessionConfiguration _sessionConfiguration;

        // A function to be called with the EventPipeEventSource _before_
        // the call to `source.Process()`.  The function should return another
        // function that will be called to check whether the optional test was validated.
        // Example in situ: providervalidation.cs
        private Func<EventPipeEventSource, Func<int>> _optionalTraceValidator;

        IpcTraceTest(
            Dictionary<string, ExpectedEventCount> expectedEventCounts,
            Action eventGeneratingAction,
            SessionConfiguration? sessionConfiguration = null,
            Func<EventPipeEventSource, Func<int>> optionalTraceValidator = null)
        {
            _eventGeneratingAction = eventGeneratingAction;
            _expectedEventCounts = expectedEventCounts;
            _sessionConfiguration = sessionConfiguration?.InjectSentinel() ?? new SessionConfiguration(
                circularBufferSizeMB: 1000,
                format: EventPipeSerializationFormat.NetTrace,
                providers: new List<Provider> { 
                    new Provider("Microsoft-Windows-DotNETRuntime"),
                    new Provider("SentinelEventSource")
                });
            _optionalTraceValidator = optionalTraceValidator;
        }

        private int Fail(string message = "")
        {
            Logger.logger.Log("Test FAILED!");
            Logger.logger.Log(message);
            Logger.logger.Log("Configuration:");
            Logger.logger.Log("{");
            Logger.logger.Log($"\tbufferSize: {_sessionConfiguration.CircularBufferSizeInMB},");
            Logger.logger.Log("\tproviders: [");
            foreach (var provider in _sessionConfiguration.Providers)
            {
                Logger.logger.Log($"\t\t{provider.ToString()},");
            }
            Logger.logger.Log("\t]");
            Logger.logger.Log("}\n");
            Logger.logger.Log("Expected:");
            Logger.logger.Log("{");
            foreach (var (k, v) in _expectedEventCounts)
            {
                Logger.logger.Log($"\t\"{k}\" = {v}");
            }
            Logger.logger.Log("}\n");

            Logger.logger.Log("Actual:");
            Logger.logger.Log("{");
            foreach (var (k, v) in _actualEventCounts)
            {
                Logger.logger.Log($"\t\"{k}\" = {v}");
            }
            Logger.logger.Log("}");

            return -1;
        }

        private int Validate()
        {
            var processId = Process.GetCurrentProcess().Id;
            Logger.logger.Log("Connecting to EventPipe...");
            var binaryReader = EventPipeClient.CollectTracing(processId, _sessionConfiguration, out var eventpipeSessionId);
            if (eventpipeSessionId == 0)
            {
                Logger.logger.Log("Failed to connect to EventPipe!");
                return -1;
            }
            Logger.logger.Log($"Connected to EventPipe with sessionID '0x{eventpipeSessionId:x}'");
            
            // CollectTracing returns before EventPipe::Enable has returned, so the
            // the sources we want to listen for may not have been enabled yet.
            // We'll use this sentinel EventSource to check if Enable has finished
            ManualResetEvent sentinelEventReceived = new ManualResetEvent(false);
            var sentinelTask = new Task(() =>
            {
                Logger.logger.Log("Started sending sentinel events...");
                while (!sentinelEventReceived.WaitOne(50))
                {
                    SentinelEventSource.Log.SentinelEvent();
                }
                Logger.logger.Log("Stopped sending sentinel events");
            });
            sentinelTask.Start();

            EventPipeEventSource source = null;
            Func<int> optionalTraceValidationCallback = null;
            var readerTask = new Task(() =>
            {
                Logger.logger.Log("Creating EventPipeEventSource...");
                source = new EventPipeEventSource(binaryReader);
                Logger.logger.Log("EventPipeEventSource created");

                source.Dynamic.All += (eventData) =>
                {
                    try
                    {
                        if (eventData.ProviderName == "SentinelEventSource")
                        {
                            if (!sentinelEventReceived.WaitOne(0))
                                Logger.logger.Log("Saw sentinel event");
                            sentinelEventReceived.Set();
                        }

                        else if (_actualEventCounts.TryGetValue(eventData.ProviderName, out _))
                        {
                            _actualEventCounts[eventData.ProviderName]++;
                        }
                        else
                        {
                            Logger.logger.Log($"Saw new provider '{eventData.ProviderName}'");
                            _actualEventCounts[eventData.ProviderName] = 1;
                        }
                    }
                    catch (Exception e)
                    {
                        Logger.logger.Log("Exception in Dynamic.All callback " + e.ToString());
                    }
                };
                Logger.logger.Log("Dynamic.All callback registered");

                if (_optionalTraceValidator != null)
                {
                    Logger.logger.Log("Running optional trace validator");
                    optionalTraceValidationCallback = _optionalTraceValidator(source);
                    Logger.logger.Log("Finished running optional trace validator");
                }

                Logger.logger.Log("Starting stream processing...");
                source.Process();
                Logger.logger.Log("Stopping stream processing");
                Logger.logger.Log($"Dropped {source.EventsLost} events");
            });

            readerTask.Start();
            sentinelEventReceived.WaitOne();

            Logger.logger.Log("Starting event generating action...");
            _eventGeneratingAction();
            Logger.logger.Log("Stopping event generating action");

            Logger.logger.Log("Sending StopTracing command...");
            EventPipeClient.StopTracing(processId, eventpipeSessionId);
            Logger.logger.Log("Finished StopTracing command");

            readerTask.Wait();
            Logger.logger.Log("Reader task finished");

            foreach (var (provider, expectedCount) in _expectedEventCounts)
            {
                if (_actualEventCounts.TryGetValue(provider, out var actualCount))
                {
                    if (!expectedCount.Validate(actualCount))
                    {
                        return Fail($"Event count mismatch for provider \"{provider}\": expected {expectedCount}, but saw {actualCount}");
                    }
                }
                else
                {
                    return Fail($"No events for provider \"{provider}\"");
                }
            }

            if (optionalTraceValidationCallback != null)
            {
                Logger.logger.Log("Validating optional callback...");
                return optionalTraceValidationCallback();
            }
            else
            {
                return 100;
            }
        }

        public static int RunAndValidateEventCounts(
            Dictionary<string, ExpectedEventCount> expectedEventCounts,
            Action eventGeneratingAction,
            SessionConfiguration? sessionConfiguration = null,
            Func<EventPipeEventSource, Func<int>> optionalTraceValidator = null)
        {
            Logger.logger.Log("==TEST STARTING==");
            var test = new IpcTraceTest(expectedEventCounts, eventGeneratingAction, sessionConfiguration, optionalTraceValidator);
            try
            {
                var ret = test.Validate();
                if (ret == 100)
                    Logger.logger.Log("==TEST FINISHED: PASSED!==");
                else
                    Logger.logger.Log("==TEST FINISHED: FAILED!==");
                return ret;
            }
            catch (Exception e)
            {
                Logger.logger.Log(e.ToString());
                Logger.logger.Log("==TEST FINISHED: FAILED!==");
                return -1;
            }
        }
    }
}