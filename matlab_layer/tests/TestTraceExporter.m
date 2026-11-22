classdef TestTraceExporter < matlab.unittest.TestCase
    %TESTTRACEEXPORTER Unit & regression tests for TraceExporter and instrumented Simulation.

    properties
        hybridRoot
        matlabDir
    end

    methods (TestClassSetup)
        function setupPaths(testCase)
            testsDir = fileparts(mfilename('fullpath'));
            testCase.matlabDir = fileparts(testsDir);
            testCase.hybridRoot = fileparts(testCase.matlabDir);
            addpath(genpath(testCase.matlabDir));
        end
    end

    methods (Test)
        function testDeterministicExport(testCase)
            % Run DTHSBP twice at N=50, seed=42 and assert identical deterministic trace fields.
            doc1 = testCase.runSingleExport('DTHSBP', 50, 42, 60);
            doc2 = testCase.runSingleExport('DTHSBP', 50, 42, 60);

            tmp1 = [tempname, '.json'];
            tmp2 = [tempname, '.json'];
            c1 = onCleanup(@() testCase.deleteIfExists(tmp1)); %#ok<NASGU>
            c2 = onCleanup(@() testCase.deleteIfExists(tmp2)); %#ok<NASGU>

            % Verify identical re-serialization of same document
            TraceExporter.exportTrace(doc1, tmp1, true);
            TraceExporter.exportTrace(doc1, tmp2, true);
            rawA = fileread(tmp1);
            rawB = fileread(tmp2);
            testCase.verifyEqual(rawA, rawB);

            % Normalize non-deterministic wall-clock metadata/timers across independent runs
            doc1.meta.generatedAt = 'FIXED';
            doc2.meta.generatedAt = 'FIXED';
            for i = 1:numel(doc1.events)
                doc1.events{i}.crypto.leaderTimeMs = 0.0;
            end
            for i = 1:numel(doc2.events)
                doc2.events{i}.crypto.leaderTimeMs = 0.0;
            end

            str1 = jsonencode(doc1, 'PrettyPrint', true);
            str2 = jsonencode(doc2, 'PrettyPrint', true);
            testCase.verifyEqual(str1, str2);
        end

        function testSpatialCoordinateBounds(testCase)
            % Verify specular boundary projection and exported node coordinates stay in bounds.
            cases = {[-150, 2350], [1200, -50], [0, 1000], [1000, 0], [3456.7, -891.2]};
            for i = 1:numel(cases)
                p = TraceExporter.projectPosition(cases{i}, 1000, 1000);
                testCase.verifyGreaterThanOrEqual(p(1), 0);
                testCase.verifyLessThanOrEqual(p(1), 1000);
                testCase.verifyGreaterThanOrEqual(p(2), 0);
                testCase.verifyLessThanOrEqual(p(2), 1000);
            end

            % Verify actual sample trace coordinates fit inside [0, arenaWidthM] x [0, arenaHeightM]
            sampleFile = fullfile(testCase.hybridRoot, 'traces', 'samples', 'events_trace_DTHSBP_N50.json');
            data = jsondecode(fileread(sampleFile));
            w = data.meta.arenaWidthM;
            h = data.meta.arenaHeightM;

            for i = 1:numel(data.initialNodes)
                node = data.initialNodes(i);
                testCase.verifyGreaterThanOrEqual(node.x, 0);
                testCase.verifyLessThanOrEqual(node.x, w);
                testCase.verifyGreaterThanOrEqual(node.y, 0);
                testCase.verifyLessThanOrEqual(node.y, h);
            end

            for i = 1:numel(data.events)
                ev = data.events(i);
                for j = 1:numel(ev.nodePositions)
                    np = ev.nodePositions(j);
                    testCase.verifyGreaterThanOrEqual(np.x, 0);
                    testCase.verifyLessThanOrEqual(np.x, w);
                    testCase.verifyGreaterThanOrEqual(np.y, 0);
                    testCase.verifyLessThanOrEqual(np.y, h);
                end
            end
        end

        function testMembershipAndDepartureConsistency(testCase)
            % Verify join/leave/failure/dtPredictedDeparture transitions and DTHSBP batching.
            doc = testCase.runSingleExport('DTHSBP', 50, 42, 100);

            clusterMembers = containers.Map('KeyType', 'double', 'ValueType', 'any');
            for i = 1:numel(doc.initialNodes)
                n = doc.initialNodes{i};
                cID = double(n.clusterId);
                if isKey(clusterMembers, cID)
                    clusterMembers(cID) = [clusterMembers(cID), double(n.uavId)];
                else
                    clusterMembers(cID) = double(n.uavId);
                end
            end

            collateralBatchFound = false;
            for i = 1:numel(doc.events)
                ev = doc.events{i};
                cID = double(ev.clusterId);
                aff = double(cell2mat(ev.affectedUAVs));
                cur = clusterMembers(cID);

                switch ev.type
                    case 'join'
                        expected = union(cur, aff, 'stable');
                    case {'leave', 'failure', 'dtPredictedDeparture'}
                        expected = setdiff(cur, aff, 'stable');
                        if ev.isCollateralBatch && numel(aff) >= 2
                            collateralBatchFound = true;
                        end
                    case 'dtDisturbance'
                        expected = cur;
                    otherwise
                        error('Unexpected event type: %s', ev.type);
                end

                if isempty(ev.clusterMembershipAfter)
                    actual = zeros(1, 0);
                else
                    actual = double(cell2mat(ev.clusterMembershipAfter));
                end

                testCase.verifyEqual(sort(actual), sort(expected), ...
                    sprintf('Membership mismatch at event %d (%s, t=%.2f)', i, ev.type, ev.t));
                clusterMembers(cID) = actual;
            end

            testCase.verifyTrue(collateralBatchFound, ...
                'Expected DTHSBP at N=50, T=100 to exhibit at least one collateral batch leave (>=2 UAVs)');
        end

        function testAllFiveProtocolsExport(testCase)
            % Verify all 5 protocols produce non-empty valid traces with exact byte formulas.
            protocols = {'FlatSBP', 'HSBP', 'DTHSBP', 'LKH', 'TGDH'};
            bytesPerMsg = struct('FlatSBP', 56, 'HSBP', 56, 'DTHSBP', 56, 'LKH', 48, 'TGDH', 128);

            for p = 1:numel(protocols)
                proto = protocols{p};
                f = fullfile(testCase.hybridRoot, 'traces', 'samples', ...
                    sprintf('events_trace_%s_N50.json', proto));
                testCase.verifyTrue(isfile(f), sprintf('Missing trace fixture: %s', f));
                data = jsondecode(fileread(f));

                testCase.verifyEqual(char(data.meta.protocol), proto);
                testCase.verifyEqual(numel(data.initialNodes), 50);
                testCase.verifyGreaterThan(numel(data.events), 0);

                rekeyEventsFound = 0;
                for i = 1:numel(data.events)
                    ev = data.events(i);
                    if ev.crypto.rekeyCount > 0
                        rekeyEventsFound = rekeyEventsFound + 1;
                        testCase.verifyGreaterThan(ev.crypto.messagesSent, 0);
                        expectedBytes = ev.crypto.messagesSent * bytesPerMsg.(proto);
                        testCase.verifyEqual(double(ev.crypto.bytesTransmitted), double(expectedBytes), ...
                            sprintf('Unexpected bytesTransmitted on %s event %d', proto, i));
                        testCase.verifyGreaterThan(numel(ev.hops), 0);
                    end
                end
                testCase.verifyGreaterThan(rekeyEventsFound, 0);
            end
        end

        function testBoundedNodePositionsScoping(testCase)
            % Verify bounded nodePositions invariant at N=250 across all 5 protocols.
            protocols = {'FlatSBP', 'HSBP', 'DTHSBP', 'LKH', 'TGDH'};
            for p = 1:numel(protocols)
                proto = protocols{p};
                f = fullfile(testCase.hybridRoot, 'traces', 'runs', ...
                    sprintf('events_trace_%s_N250.json', proto));
                testCase.verifyTrue(isfile(f), sprintf('Missing N=250 trace: %s', f));
                data = jsondecode(fileread(f));

                for i = 1:numel(data.events)
                    ev = data.events(i);
                    nPos = numel(ev.nodePositions);
                    nMem = numel(ev.clusterMembershipAfter);
                    testCase.verifyLessThanOrEqual(nPos, nMem + 1, ...
                        sprintf('%s event %d exceeded bounded nodePositions limit (%d > %d+1)', proto, i, nPos, nMem));

                    for j = 1:nPos
                        np = ev.nodePositions(j);
                        isTargetOrSL = (double(np.clusterId) == double(ev.clusterId)) || strcmp(char(np.role), 'SL');
                        testCase.verifyTrue(isTargetOrSL, ...
                            sprintf('%s event %d included foreign non-SL node uavId=%d (cluster %d ~= %d)', ...
                            proto, i, np.uavId, np.clusterId, ev.clusterId));
                    end
                end
            end
        end

        function testDTHSBPLifespanSortingSavings(testCase)
            % Verify Contribution A O(1) tail truncation on predicted leaves at N=250.
            dtFile = fullfile(testCase.hybridRoot, 'traces', 'runs', 'events_trace_DTHSBP_N250.json');
            hsFile = fullfile(testCase.hybridRoot, 'traces', 'runs', 'events_trace_HSBP_N250.json');
            testCase.verifyTrue(isfile(dtFile));
            testCase.verifyTrue(isfile(hsFile));

            dtData = jsondecode(fileread(dtFile));
            hsData = jsondecode(fileread(hsFile));

            dtTotalBytes = 0;
            dtDepBytes = 0;
            hsTotalBytes = 0;
            hsDepBytes = 0;
            hitCount = 0;

            for i = 1:numel(dtData.events)
                ev = dtData.events(i);
                b = double(ev.crypto.bytesTransmitted);
                dtTotalBytes = dtTotalBytes + b;
                if ismember(char(ev.type), {'leave', 'dtPredictedDeparture', 'failure'})
                    dtDepBytes = dtDepBytes + b;
                    if islogical(ev.predictionHit) && ev.predictionHit
                        hitCount = hitCount + 1;
                        testCase.verifyEqual(double(ev.crypto.messagesSent), 1, ...
                            sprintf('Expected O(1)=1 msg on predictionHit at t=%.2f', ev.t));
                        testCase.verifyEqual(b, 56, ...
                            sprintf('Expected 56 B on predictionHit at t=%.2f', ev.t));
                        testCase.verifyEqual(double(ev.hops(2).bytes), 56);
                        testCase.verifyEqual(double(ev.hops(2).fragmentsCount), 1);
                    end
                end
            end

            for i = 1:numel(hsData.events)
                ev = hsData.events(i);
                b = double(ev.crypto.bytesTransmitted);
                hsTotalBytes = hsTotalBytes + b;
                if ismember(char(ev.type), {'leave', 'dtPredictedDeparture', 'failure'})
                    hsDepBytes = hsDepBytes + b;
                end
            end

            depReduction = 1.0 - (dtDepBytes / hsDepBytes);
            totalReduction = 1.0 - (dtTotalBytes / hsTotalBytes);
            testCase.verifyGreaterThan(hitCount, 15);
            testCase.verifyGreaterThanOrEqual(depReduction, 0.70, ...
                sprintf('Expected >=70%% departure byte reduction vs HSBP, got %.2f%%', depReduction * 100));
            testCase.verifyGreaterThanOrEqual(totalReduction, 0.35, ...
                sprintf('Expected >=35%% total byte reduction vs HSBP, got %.2f%%', totalReduction * 100));
        end
    end

    methods (Access = private)
        function doc = runSingleExport(~, protoName, N, seed, simTime)
            cfg = config();
            cfg.numUAVs = N;
            cfg.numClusters = 5;
            cfg.clusterSize = round(N / cfg.numClusters);
            cfg.simulationTime = simTime;
            cfg.randomSeed = seed;
            cfg.enableTraceExport = true;

            rng(cfg.randomSeed, 'twister');
            swarm = Swarm(cfg);
            switch protoName
                case 'DTHSBP'
                    protocol = DTHSBP(swarm);
                case 'HSBP'
                    protocol = HSBP(swarm);
                case 'FlatSBP'
                    protocol = FlatSBP(swarm);
                case 'LKH'
                    protocol = TreeProtocolTraceAdapter(swarm, 'LKH', 3);
                case 'TGDH'
                    protocol = TreeProtocolTraceAdapter(swarm, 'TGDH', 2);
            end

            sim = Simulation(cfg, protocol);
            sim.run();
            doc = TraceExporter.buildTraceDocument( ...
                sim.cfg, sim.protocolName, sim.initialNodesSnapshot, sim.traceLog);
        end

        function deleteIfExists(~, f)
            if isfile(f)
                delete(f);
            end
        end
    end
end
