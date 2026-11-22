classdef Simulation < handle
    
    properties
        swarm
        protocol
        cfg
        eventQueue EventQueue
        eventGenerator
        eventFactory
        dtDisturbanceGenerator
        dtManager
        statistics
        precomputationEngine
        currentTime = 0
        dtDisturbanceCount = 0
        generatedJoinEvents = 0
        generatedLeaveEvents = 0
        generatedFailureEvents = 0
        dtDrivenLeaveEvents = 0
        dtCollateralDepartureEvents = 0
        traceLog = {}
        initialNodesSnapshot = {}
        protocolName = ""
    end
    
    methods
        
        function obj = Simulation(cfg, protocol)
            obj.cfg = cfg;
            obj.protocol = protocol;
            obj.swarm = protocol.swarm;
            obj.currentTime = 0;
            
            obj.eventQueue = EventQueue();
            obj.eventGenerator = PoissonEventGenerator(obj.cfg);
            obj.eventFactory = EventFactory(obj.swarm);
            obj.dtDisturbanceGenerator = DTDisturbanceGenerator(obj.cfg);
            obj.dtManager = DigitalTwinManager(obj.cfg);
            
            if isa(obj.protocol, "DTHSBP") && isprop(obj.protocol, "precomputationEngine") && ~isempty(obj.protocol.precomputationEngine)
                obj.precomputationEngine = obj.protocol.precomputationEngine;
            else
                obj.precomputationEngine = DTPrecomputationEngine(obj.cfg, obj.swarm, obj.protocol.engine);
            end
            
            initialUAVs = obj.swarm.getActiveUAVs();
            for i = 1:numel(initialUAVs)
                obj.dtManager.registerUAV(initialUAVs(i));
            end
            
            obj.statistics = StatisticsManager(obj.cfg);
            
            obj.protocolName = string(TraceExporter.resolveProtocolName(obj.protocol));
            obj.traceLog = {};
            if obj.isTraceExportEnabled()
                obj.initialNodesSnapshot = TraceExporter.captureInitialNodes(obj.swarm, obj.cfg);
            else
                obj.initialNodesSnapshot = {};
            end
        end
        
        function step(obj)
            obj.cfg.currentTime = obj.currentTime;
            
            suppressMembershipEvents = false;
            if isfield(obj.cfg,'dtIntegrationSuppressMembershipEvents')
                suppressMembershipEvents = logical(obj.cfg.dtIntegrationSuppressMembershipEvents);
            end
            
            if suppressMembershipEvents
                eventCounts.join = 0;
                eventCounts.leave = 0;
                eventCounts.failure = 0;
            else
                eventCounts = obj.eventGenerator.generate();
            end
            
            obj.generatedJoinEvents = obj.generatedJoinEvents + eventCounts.join;
            obj.generatedLeaveEvents = obj.generatedLeaveEvents + eventCounts.leave;
            obj.generatedFailureEvents = obj.generatedFailureEvents + eventCounts.failure;
            
            events = obj.eventFactory.createEvents(eventCounts,obj.currentTime);
            for i = 1:numel(events)
                obj.eventQueue.schedule(events{i});
            end
            
            disturbanceCount = obj.dtDisturbanceGenerator.generate();
            disturbanceEvents = obj.createDisturbanceEvents(disturbanceCount);
            for i = 1:numel(disturbanceEvents)
                obj.eventQueue.schedule(disturbanceEvents{i});
            end
            
            obj.processEvents();
            
            obj.swarm.step(obj.cfg);
            obj.currentTime = obj.currentTime + obj.cfg.timeStep;
            obj.cfg.currentTime = obj.currentTime;
            obj.updateDigitalTwins();
            obj.schedulePredictedDepartures();
        end
        
        function events = createDisturbanceEvents(obj,count)
            events = {};
            if count <= 0
                return;
            end
            
            for k = 1:count
                cluster = obj.selectDisturbanceCluster();
                if isempty(cluster)
                    continue;
                end
                
                activeUAVs = cluster.getActiveUAVs();
                if isempty(activeUAVs)
                    continue;
                end
                
                requestedCount = max(1,floor(obj.cfg.dtDisturbanceUAVCount));
                selectedCount = min(requestedCount,numel(activeUAVs));
                if selectedCount == numel(activeUAVs)
                    selected = activeUAVs;
                else
                    selected = activeUAVs(randperm(numel(activeUAVs),selectedCount));
                end
                
                ids = [selected.id];
                events{end+1} = DTDisturbanceEvent( ...
                    obj.currentTime,cluster.id,ids, ...
                    obj.cfg.dtDisturbancePositionStep, ...
                    obj.cfg.dtDisturbanceVelocityStep, ...
                    obj.cfg.dtDisturbanceEnergyDrop);
            end
            
            obj.dtDisturbanceCount = obj.dtDisturbanceCount + numel(events);
        end
        
        function cluster = selectDisturbanceCluster(obj)
            eligible = [];
            for i = 1:numel(obj.swarm.clusters)
                if ~isempty(obj.swarm.clusters(i).getActiveUAVs())
                    eligible(end+1) = i;
                end
            end
            
            if isempty(eligible)
                cluster = [];
                return;
            end
            
            index = eligible(randi(numel(eligible)));
            cluster = obj.swarm.clusters(index);
        end
        
        function processPredictedDeparture(obj,uavID,reason)
            uav = obj.swarm.findUAV(uavID);
            if isempty(uav) || ~uav.active
                return;
            end
            
            obj.dtDrivenLeaveEvents = obj.dtDrivenLeaveEvents + 1;
            obj.processLeaveRequest(uavID,"dtDrivenLeave");
        end
        
        function schedulePredictedDepartures(obj)
            traceEnabled = obj.isTraceExportEnabled();
            if ~traceEnabled && ~isa(obj.protocol,"DTHSBP")
                return;
            end
            
            enabled = false;
            if isfield(obj.cfg,'dtPredictedDepartureEnabled')
                enabled = logical(obj.cfg.dtPredictedDepartureEnabled);
            end
            if ~enabled
                return;
            end
            
            currentTime = obj.currentTime;
            departureTime = currentTime + obj.cfg.predictionHorizon;
            activeUAVs = obj.swarm.getActiveUAVs();
            
            baseSeed = 42;
            if isfield(obj.cfg, 'randomSeed') && ~isempty(obj.cfg.randomSeed)
                baseSeed = round(double(obj.cfg.randomSeed));
            end
            
            if traceEnabled
                sPrev = rng;
                restoreRng = onCleanup(@() rng(sPrev)); %#ok<NASGU>
            end
            
            for i = 1:numel(activeUAVs)
                uav = activeUAVs(i);
                if isempty(uav.dt) || ~uav.dt.predictedLeave || isnan(uav.dt.predictionTime)
                    continue;
                end
                if abs(uav.dt.predictionTime - currentTime) > 1e-12
                    continue;
                end
                
                probability = obj.computeDTDepartureProbability( ...
                    uav.dt.stabilityScore,obj.cfg.predictionHorizon);
                
                if traceEnabled
                    uavSeed = mod(baseSeed + (round(currentTime * 1000) + 1) * 3000003 + round(double(uav.id)) * 97, 4294967291);
                    rng(uavSeed, 'twister');
                end
                
                if rand() > probability
                    uav.dt.cancelPrediction();
                    continue;
                end
                
                obj.statistics.incrementDTPredictionsRealized();
                obj.eventQueue.schedule(DTPredictedDepartureEvent( ...
                    departureTime,uav.id,"DT-predicted instability"));
                
                if traceEnabled && ~isa(obj.protocol, "DTHSBP")
                    uav.dt.cancelPrediction();
                end
            end
        end
        
        function probability = computeDTDepartureProbability(obj,stabilityScore,horizon)
            validateattributes(stabilityScore,{'numeric'},{'scalar','real','finite','nonnegative'});
            validateattributes(horizon,{'numeric'},{'scalar','real','finite','nonnegative'});
            
            if isfield(obj.cfg,'dtPredictedDepartureProbability') && ...
                    ~isempty(obj.cfg.dtPredictedDepartureProbability)
                probability = max(0, min(1, double(obj.cfg.dtPredictedDepartureProbability)));
                return;
            end
            
            if ~isfield(obj.cfg,'dtDepartureHazardModel') || ...
                    string(obj.cfg.dtDepartureHazardModel) ~= "threshold_anchored_exponential"
                error('Simulation:UnsupportedDTHazardModel','Unsupported DT departure hazard model.');
            end
            
            theta = obj.cfg.thetaLeave;
            leaveRate = obj.cfg.leaveRate;
            if stabilityScore <= 0 || theta <= 0 || leaveRate <= 0 || horizon <= 0
                probability = 0;
                return;
            end
            
            hazard = leaveRate * exp(stabilityScore/theta - 1);
            probability = 1 - exp(-hazard*horizon);
            probability = max(0,min(1,probability));
        end
        
        function run(obj)
            while obj.currentTime < obj.cfg.simulationTime
                obj.step();
            end
        end
        
        function tf = isTraceExportEnabled(obj)
            tf = isfield(obj.cfg, 'enableTraceExport') && logical(obj.cfg.enableTraceExport);
        end
        
        function exportTrace(obj, filepath, prettyPrint)
            if nargin < 3
                prettyPrint = true;
            end
            doc = TraceExporter.buildTraceDocument( ...
                obj.cfg, obj.protocolName, obj.initialNodesSnapshot, obj.traceLog);
            TraceExporter.exportTrace(doc, filepath, prettyPrint);
        end
        
        function recordTraceEvent(obj, eventType, clusterID, affectedUAVs, isCollateralBatch, predictionHit, rekeyResult)
            if ~obj.isTraceExportEnabled()
                return;
            end
            treeDeg = TraceExporter.resolveTreeDegree(obj.protocol);
            rec = TraceExporter.buildTraceRecord( ...
                obj.currentTime, eventType, clusterID, affectedUAVs, ...
                isCollateralBatch, predictionHit, rekeyResult, ...
                obj.protocolName, obj.swarm, obj.cfg, treeDeg);
            obj.traceLog{end+1} = rec;
        end
        
        function processEvents(obj)
            dueEvents = obj.eventQueue.popDueEvents(obj.currentTime);
            for i = 1:numel(dueEvents)
                ev = dueEvents{i};
                ev.execute(obj);
                if obj.isTraceExportEnabled() && isa(ev, "DTDisturbanceEvent")
                    obj.recordTraceEvent("dtDisturbance", ev.clusterID, ev.uavIDs, false, NaN, []);
                end
            end
        end
        
        function processJoinRequest(obj,uavID,clusterID)
            rekeyResult = obj.protocol.join(uavID,clusterID);
            if isempty(rekeyResult)
                obj.statistics.incrementRejectedJoin();
                return;
            end
            
            uav = obj.swarm.findUAV(uavID);
            if ~isempty(uav)
                obj.dtManager.registerUAV(uav);
            end
            
            obj.statistics.incrementJoin();
            obj.statistics.recordRekey(false,rekeyResult,"join");
            
            if obj.isTraceExportEnabled()
                hitVal = NaN;
                obj.recordTraceEvent("join", clusterID, uavID, false, hitVal, rekeyResult);
            end
        end
        
        function processLeaveRequest(obj,uavID,eventType)
            if nargin < 3 || isempty(eventType)
                eventType = "leave";
            else
                eventType = string(eventType);
            end
            
            decision = obj.makeLeaveDecision(uavID);
            if ~decision.approved
                return;
            end
            
            uav = obj.swarm.findUAV(uavID);
            if isempty(uav)
                return;
            end
            triggerClusterID = uav.clusterID;
            
            rekeyResult = obj.protocol.leave(uavID);
            obj.dtManager.removeUAV(uavID);
            
            collateralCount = 0;
            if ~isempty(rekeyResult)
                predictedLeaves = rekeyResult.predictedLeaves;
                for i = 1:numel(predictedLeaves)
                    obj.dtManager.removeUAV(predictedLeaves(i));
                end
                collateralCount = numel(predictedLeaves);
            end
            
            obj.statistics.incrementLeave();
            
            if ~isempty(rekeyResult)
                if collateralCount > 0
                    obj.dtCollateralDepartureEvents = ...
                        obj.dtCollateralDepartureEvents + collateralCount;
                    obj.statistics.incrementPredictedLeaves(collateralCount);
                    obj.statistics.recordRekey(true,rekeyResult,"dtBatch");
                elseif eventType == "dtDrivenLeave"
                    obj.statistics.recordRekey(true,rekeyResult,"dtDrivenLeave");
                else
                    obj.statistics.recordRekey(false,rekeyResult,"leave");
                end
            end
            
            if obj.isTraceExportEnabled()
                if eventType == "dtDrivenLeave"
                    traceType = "dtPredictedDeparture";
                else
                    traceType = "leave";
                end
                
                affected = double(uavID);
                isCollateral = false;
                if collateralCount > 0
                    affected = unique([double(uavID), double(rekeyResult.predictedLeaves(:).')], 'stable');
                    isCollateral = true;
                end
                
                hitVal = NaN;
                if ~isempty(rekeyResult) && strcmp(char(obj.protocolName), "DTHSBP")
                    hitVal = logical(rekeyResult.isPrecomputedHit);
                end
                
                obj.recordTraceEvent(traceType, triggerClusterID, affected, isCollateral, hitVal, rekeyResult);
            end
        end
        
        function processFailure(obj,uavID,reason)
            decision = obj.makeFailureDecision(uavID,reason);
            if ~decision.approved
                return;
            end
            
            uav = obj.swarm.findUAV(uavID);
            if isempty(uav)
                return;
            end
            triggerClusterID = uav.clusterID;
            
            rekeyResult = obj.protocol.failure(uavID);
            obj.dtManager.removeUAV(uavID);
            
            collateralCount = 0;
            if ~isempty(rekeyResult)
                predictedLeaves = rekeyResult.predictedLeaves;
                for i = 1:numel(predictedLeaves)
                    obj.dtManager.removeUAV(predictedLeaves(i));
                end
                collateralCount = numel(predictedLeaves);
            end
            
            obj.statistics.incrementFailure();
            
            if ~isempty(rekeyResult)
                if collateralCount > 0
                    obj.dtCollateralDepartureEvents = ...
                        obj.dtCollateralDepartureEvents + collateralCount;
                    obj.statistics.incrementPredictedLeaves(collateralCount);
                    obj.statistics.recordRekey(true,rekeyResult,"dtBatch");
                else
                    obj.statistics.recordRekey(false,rekeyResult,"failure");
                end
            end
            
            if obj.isTraceExportEnabled()
                affected = double(uavID);
                isCollateral = false;
                if collateralCount > 0
                    affected = unique([double(uavID), double(rekeyResult.predictedLeaves(:).')], 'stable');
                    isCollateral = true;
                end
                
                hitVal = NaN;
                if ~isempty(rekeyResult) && strcmp(char(obj.protocolName), "DTHSBP")
                    hitVal = logical(rekeyResult.isPrecomputedHit);
                end
                
                obj.recordTraceEvent("failure", triggerClusterID, affected, isCollateral, hitVal, rekeyResult);
            end
        end
        
        function result = makeAdmissionDecision(obj,candidateUAV)
            result.accepted = true;
            result.reason = "Accepted";
        end
        
        function decision = makeLeaveDecision(obj,uavID)
            decision.approved = true;
            decision.reason = "Approved";
        end
        
        function decision = makeFailureDecision(obj,uavID,reason)
            decision.approved = true;
            decision.reason = "Approved";
        end
        
        function updateDigitalTwins(obj)
            activeUAVs = obj.swarm.getActiveUAVs();
            for i = 1:numel(activeUAVs)
                activeUAVs(i).dt.cfg.currentTime = obj.currentTime;
            end
            
            previousPredictions = false(1,numel(activeUAVs));
            for i = 1:numel(activeUAVs)
                previousPredictions(i) = activeUAVs(i).dt.predictedLeave;
            end
            
            obj.dtManager.update();
            
            for i = 1:numel(activeUAVs)
                if ~previousPredictions(i) && activeUAVs(i).dt.predictedLeave
                    obj.statistics.incrementDTPredictionsCreated();
                end
            end
            
            % Contribution B: Asynchronously pre-compute key packages for predicted departures
            if isa(obj.protocol, "DTHSBP") && ~isempty(obj.precomputationEngine)
                obj.precomputationEngine.interceptAllPredictions(obj.dtManager);
            end
        end
        
        function printStatistics(obj)
            stats = obj.statistics.getStatistics();
            fprintf('\n');
            fprintf('========== Simulation Statistics ==========\n');
            fprintf('Join Events        : %d\n',stats.joinEvents);
            fprintf('Leave Events       : %d\n',stats.leaveEvents);
            fprintf('Failure Events     : %d\n',stats.failureEvents);
            fprintf('DT Disturbances    : %d\n',obj.dtDisturbanceCount);
            fprintf('DT Predictions     : %d\n',stats.dtPredictionsCreated);
            fprintf('DT Realized        : %d\n',stats.dtPredictionsRealized);
            fprintf('DT Unrealized      : %d\n',stats.dtPredictionsUnrealized);
            fprintf('DT Realization     : %.6f\n',stats.dtRealizationRatio);
            fprintf('Precomputation Hits: %d\n',stats.precomputationHits);
            fprintf('Precomp Hit Rate   : %.2f%%\n',stats.precomputationHitRate * 100);
            fprintf('Leader Reduction   : %.6f s\n',stats.leaderTimeReduction);
            fprintf('Predicted Leaves   : %d\n',stats.predictedLeaves);
            fprintf('DT Driven Leaves   : %d\n',obj.dtDrivenLeaveEvents);
            fprintf('DT Collateral      : %d\n',obj.dtCollateralDepartureEvents);
            fprintf('Rejected Joins     : %d\n',stats.rejectedJoins);
            fprintf('Local Rekeys       : %d\n',stats.localRekeys);
            fprintf('Batch Rekeys       : %d\n',stats.batchRekeys);
            fprintf('Join Messages      : %d\n',stats.joinMessages);
            fprintf('Leave Messages     : %d\n',stats.leaveMessages);
            fprintf('Failure Messages   : %d\n',stats.failureMessages);
            fprintf('DT Leave Messages  : %d\n',stats.dtDrivenLeaveMessages);
            fprintf('DT Batch Messages  : %d\n',stats.dtBatchMessages);
            fprintf('Messages Sent      : %d\n',stats.totalMessages);
            fprintf('Bytes Transmitted  : %d\n',stats.totalBytes);
            fprintf('Communication Cost : %.0f\n',stats.communicationCost);
            fprintf('Encryptions        : %d\n',stats.totalEncryptions);
            fprintf('Decryptions        : %d\n',stats.totalDecryptions);
            fprintf('Hash Operations    : %d\n',stats.totalHashOperations);
            fprintf('Random Numbers     : %d\n',stats.totalRandomNumbers);
            fprintf('===========================================\n');
        end
        
    end
end
