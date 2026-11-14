classdef TraceExporter
    %TRACEEXPORTER Deterministic structured JSON trace exporter for DT-HSBP.
    %   Converts MATLAB Simulation initial state, bounded per-event 2D UAV
    %   coordinates, cluster membership transitions, multi-hop rekeying traffic,
    %   and cryptographic costs into JSON conforming to swarm_trace_schema.json.

    methods (Static)

        function p = projectPosition(pos, areaX, areaY)
            %PROJECTPOSITION Reflect unbounded 2D position into [0, areaX] x [0, areaY].
            x = TraceExporter.reflect1D(double(pos(1)), double(areaX));
            y = TraceExporter.reflect1D(double(pos(2)), double(areaY));
            p = [x, y];
        end

        function protoName = resolveProtocolName(protocol)
            %RESOLVEPROTOCOLNAME Extract canonical schema protocol name.
            if isprop(protocol, 'protocolName') && ~isempty(protocol.protocolName)
                protoName = char(protocol.protocolName);
            else
                protoName = char(class(protocol));
            end
        end

        function deg = resolveTreeDegree(protocol)
            %RESOLVETREEDEGREE Extract tree degree for LKH/TGDH adapters.
            deg = 3;
            if isprop(protocol, 'treeDegree') && ~isempty(protocol.treeDegree)
                deg = double(protocol.treeDegree);
            end
        end

        function nodesCell = captureInitialNodes(swarm, cfg)
            %CAPTUREINITIALNODES Snapshot all initial active UAVs before simulation.
            slID = -1;
            if ~isempty(swarm.leader) && swarm.leader.active
                slID = swarm.leader.id;
            end

            total = swarm.totalUAVs();
            nodesCell = cell(1, max(0, total));
            idx = 0;

            for c = 1:numel(swarm.clusters)
                cluster = swarm.clusters(c);
                chID = -1;
                if ~isempty(cluster.head) && cluster.head.active
                    chID = cluster.head.id;
                end

                uavs = cluster.getActiveUAVs();
                for i = 1:numel(uavs)
                    u = uavs(i);
                    if u.id == slID
                        role = 'SL';
                    elseif u.id == chID
                        role = 'CH';
                    else
                        role = 'follower';
                    end

                    p = TraceExporter.projectPosition(u.position, cfg.areaX, cfg.areaY);

                    s = struct();
                    s.uavId = int64(u.id);
                    s.clusterId = int64(u.clusterID);
                    s.role = char(role);
                    s.x = double(p(1));
                    s.y = double(p(2));
                    s.vx = double(u.velocity(1));
                    s.vy = double(u.velocity(2));
                    s.energy = double(max(0, u.energy));

                    idx = idx + 1;
                    nodesCell{idx} = s;
                end
            end

            nodesCell = nodesCell(1:idx);
        end

        function [posCell, membershipCell, chFollowerCount] = buildBoundedNodePositions(swarm, clusterID, cfg)
            %BUILDBOUNDEDNODEPOSITIONS Capture only SL, cluster CH, and cluster followers.
            slUAV = swarm.leader;
            if ~isempty(slUAV) && ~slUAV.active
                slUAV = [];
            end

            cluster = swarm.findCluster(clusterID);
            activeUAVs = UAV.empty;
            chID = -1;
            if ~isempty(cluster)
                activeUAVs = cluster.getActiveUAVs();
                if ~isempty(cluster.head) && cluster.head.active
                    chID = cluster.head.id;
                end
            end

            posCell = cell(1, numel(activeUAVs) + 1);
            seenIDs = zeros(1, numel(activeUAVs) + 1);
            posIdx = 0;

            if ~isempty(slUAV)
                p = TraceExporter.projectPosition(slUAV.position, cfg.areaX, cfg.areaY);
                s = struct();
                s.uavId = int64(slUAV.id);
                s.clusterId = int64(slUAV.clusterID);
                s.role = 'SL';
                s.x = double(p(1));
                s.y = double(p(2));

                posIdx = posIdx + 1;
                posCell{posIdx} = s;
                seenIDs(posIdx) = slUAV.id;
            end

            membershipCell = cell(1, numel(activeUAVs));
            for i = 1:numel(activeUAVs)
                u = activeUAVs(i);
                membershipCell{i} = int64(u.id);

                if any(seenIDs(1:posIdx) == u.id)
                    continue;
                end

                if u.id == chID
                    role = 'CH';
                else
                    role = 'follower';
                end

                p = TraceExporter.projectPosition(u.position, cfg.areaX, cfg.areaY);
                s = struct();
                s.uavId = int64(u.id);
                s.clusterId = int64(u.clusterID);
                s.role = char(role);
                s.x = double(p(1));
                s.y = double(p(2));

                posIdx = posIdx + 1;
                posCell{posIdx} = s;
                seenIDs(posIdx) = u.id;
            end

            posCell = posCell(1:posIdx);
            chFollowerCount = max(0, numel(activeUAVs) - 1);
        end

        function bytes = computeProtocolBytes(protocolName, messagesSent, cfg)
            %COMPUTEPROTOCOLBYTES Compute transmitted bytes matching baseline model.
            proto = char(protocolName);
            msgs = max(0, round(double(messagesSent)));
            switch proto
                case 'LKH'
                    bytes = msgs * (round(double(cfg.keySize)) + round(double(cfg.headerSize)));
                case 'TGDH'
                    bytes = msgs * 128;
                otherwise
                    bytes = msgs * round(double(cfg.messageSize));
            end
        end

        function hopsCell = buildHops(protocolName, rekeyResult, cfg, eventType, followerCount, affectedCount, treeDegree)
            %BUILDHOPS Construct protocol-specific wireless hop descriptors.
            if nargin < 7 || isempty(treeDegree)
                treeDegree = 3;
            end

            proto = char(protocolName);
            evType = char(eventType);
            mtu = max(1, double(cfg.mtu));

            if strcmp(evType, 'dtDisturbance')
                nAff = max(0, round(double(affectedCount)));
                if nAff == 0
                    hopsCell = {};
                    return;
                end
                dtBytes = max(0, nAff * round(double(cfg.controlSize)));
                frag = max(1, ceil(dtBytes / mtu));
                if dtBytes == 0
                    frag = 0;
                end
                hopsCell = {TraceExporter.makeHop('follower', 'CH', dtBytes, 'FOLLOWER_TO_CH_DT', 1, frag)};
                return;
            end

            if isempty(rekeyResult) || rekeyResult.messagesSent <= 0
                hopsCell = {};
                return;
            end

            msgs = round(double(rekeyResult.messagesSent));
            totalBytes = TraceExporter.computeProtocolBytes(proto, msgs, cfg);
            fan = max(0, round(double(followerCount)));

            switch proto
                case {'DTHSBP', 'HSBP'}
                    slBytes = round(double(cfg.messageSize));
                    h1 = TraceExporter.makeHop('SL', 'CH', slBytes, 'SL_TO_CH', 1, max(1, ceil(slBytes / mtu)));
                    h2 = TraceExporter.makeHop('CH', 'follower', totalBytes, 'CH_TO_FOLLOWER', fan, max(1, ceil(totalBytes / mtu)));
                    hopsCell = {h1, h2};

                case 'FlatSBP'
                    msgBytes = round(double(cfg.messageSize));
                    singleHop = TraceExporter.makeHop('SL', 'follower', msgBytes, 'CH_TO_FOLLOWER', 1, max(1, ceil(msgBytes / mtu)));
                    hopsCell = repmat({singleHop}, 1, msgs);

                case 'LKH'
                    msgBytes = round(double(cfg.keySize)) + round(double(cfg.headerSize));
                    h1 = TraceExporter.makeHop('SL', 'CH', msgBytes, 'SL_TO_CH', 1, max(1, ceil(msgBytes / mtu)));
                    d = max(2, round(double(treeDegree)));
                    if strcmp(evType, 'join')
                        roundHop = TraceExporter.makeHop('CH', 'follower', msgBytes, 'CH_TO_FOLLOWER', fan, max(1, ceil(msgBytes / mtu)));
                        roundsCell = repmat({roundHop}, 1, msgs);
                    else
                        nFull = floor(msgs / d);
                        remMsgs = mod(msgs, d);
                        roundsCell = cell(1, nFull + double(remMsgs > 0));
                        rIdx = 0;
                        if nFull > 0
                            fullBytes = d * msgBytes;
                            fullHop = TraceExporter.makeHop('CH', 'follower', fullBytes, 'CH_TO_FOLLOWER', fan, max(1, ceil(fullBytes / mtu)));
                            roundsCell(1:nFull) = repmat({fullHop}, 1, nFull);
                            rIdx = nFull;
                        end
                        if remMsgs > 0
                            remBytes = remMsgs * msgBytes;
                            roundsCell{rIdx + 1} = TraceExporter.makeHop('CH', 'follower', remBytes, 'CH_TO_FOLLOWER', fan, max(1, ceil(remBytes / mtu)));
                        end
                    end
                    hopsCell = [{h1}, roundsCell];

                case 'TGDH'
                    tokenBytes = 128;
                    h1 = TraceExporter.makeHop('SL', 'CH', tokenBytes, 'SL_TO_CH', 1, max(1, ceil(tokenBytes / mtu)));
                    roundHop = TraceExporter.makeHop('CH', 'follower', tokenBytes, 'CH_TO_FOLLOWER', fan, max(1, ceil(tokenBytes / mtu)));
                    roundsCell = repmat({roundHop}, 1, msgs);
                    hopsCell = [{h1}, roundsCell];

                otherwise
                    error('TraceExporter:UnsupportedProtocol', 'Unsupported protocol for hop export: %s', proto);
            end
        end

        function rec = buildTraceRecord(t, eventType, clusterID, affectedUAVs, isCollateralBatch, predictionHit, rekeyResult, protocolName, swarm, cfg, treeDegree)
            %BUILDTRACERECORD Build one schema-compliant event record with bounded positions.
            if nargin < 11 || isempty(treeDegree)
                treeDegree = 3;
            end

            [posCell, membershipCell, followerCount] = TraceExporter.buildBoundedNodePositions(swarm, clusterID, cfg);
            hopsCell = TraceExporter.buildHops(protocolName, rekeyResult, cfg, eventType, followerCount, numel(affectedUAVs), treeDegree);

            if isempty(affectedUAVs)
                affCell = {};
            else
                affVec = affectedUAVs(:).';
                affCell = cell(1, numel(affVec));
                for i = 1:numel(affVec)
                    affCell{i} = int64(affVec(i));
                end
            end

            crypto = struct();
            if strcmp(char(eventType), 'dtDisturbance') || isempty(rekeyResult)
                crypto.leaderTimeMs = 0.0;
                crypto.rekeyCount = int64(0);
                crypto.messagesSent = int64(0);
                crypto.bytesTransmitted = int64(0);
            else
                msgs = max(0, round(double(rekeyResult.messagesSent)));
                bytes = TraceExporter.computeProtocolBytes(protocolName, msgs, cfg);
                crypto.leaderTimeMs = double(max(0, rekeyResult.leaderTime * 1000.0));
                crypto.rekeyCount = int64(msgs > 0);
                crypto.messagesSent = int64(msgs);
                crypto.bytesTransmitted = int64(bytes);
            end

            rec = struct();
            rec.t = double(max(0, t));
            rec.type = char(eventType);
            rec.clusterId = int64(max(1, clusterID));
            rec.affectedUAVs = affCell;
            rec.isCollateralBatch = logical(isCollateralBatch);
            if isempty(predictionHit) || (isnumeric(predictionHit) && isnan(predictionHit))
                rec.predictionHit = NaN;
            else
                rec.predictionHit = logical(predictionHit);
            end
            rec.nodePositions = posCell;
            rec.hops = hopsCell;
            rec.crypto = crypto;
            rec.clusterMembershipAfter = membershipCell;
        end

        function doc = buildTraceDocument(cfg, protocolName, initialNodesCell, eventsCell, generatedBy)
            %BUILDTRACEDOCUMENT Assemble root JSON document matching swarm_trace_schema.json.
            if nargin < 5 || isempty(generatedBy)
                generatedBy = 'DT-HSBP MATLAB Simulation (TraceExporter)';
            end

            meta = struct();
            meta.seed = int64(cfg.randomSeed);
            meta.swarmSize = int64(cfg.numUAVs);
            meta.numClusters = int64(cfg.numClusters);
            meta.protocol = char(protocolName);
            meta.simulationTime = double(cfg.simulationTime);
            meta.timeStep = double(cfg.timeStep);
            meta.arenaWidthM = double(cfg.areaX);
            meta.arenaHeightM = double(cfg.areaY);
            meta.maxVelocityMps = double(cfg.maxVelocity);
            meta.keySize = int64(cfg.keySize);
            meta.headerSize = int64(cfg.headerSize);
            meta.controlSize = int64(cfg.controlSize);
            meta.messageSize = int64(cfg.messageSize);
            meta.packetSize = int64(cfg.packetSize);
            meta.mtu = int64(cfg.mtu);
            meta.generatedBy = char(generatedBy);
            meta.generatedAt = char(datetime('now', 'TimeZone', 'UTC', 'Format', 'yyyy-MM-dd''T''HH:mm:ss''Z'''));

            if isempty(initialNodesCell)
                initialNodesCell = {};
            end
            if isempty(eventsCell)
                eventsCell = {};
            end

            doc = struct();
            doc.meta = meta;
            doc.initialNodes = initialNodesCell;
            doc.events = eventsCell;
        end

        function exportTrace(doc, filepath, prettyPrint)
            %EXPORTTRACE Serialize trace document to UTF-8 JSON file.
            if nargin < 3
                prettyPrint = true;
            end

            [parentDir, ~, ~] = fileparts(char(filepath));
            if ~isempty(parentDir) && ~exist(parentDir, 'dir')
                mkdir(parentDir);
            end

            jsonStr = jsonencode(doc, 'PrettyPrint', logical(prettyPrint));
            fid = fopen(char(filepath), 'w', 'n', 'UTF-8');
            if fid == -1
                error('TraceExporter:FileOpenFailed', 'Could not open output file: %s', char(filepath));
            end
            cleanupObj = onCleanup(@() fclose(fid));
            fwrite(fid, jsonStr, 'char');
        end

    end

    methods (Static, Access = private)

        function out = reflect1D(v, span)
            if span <= 0
                out = 0;
                return;
            end
            period = 2 * span;
            m = mod(v, period);
            if m > span
                out = period - m;
            else
                out = m;
            end
            out = min(span, max(0, out));
        end

        function h = makeHop(fromRole, toRole, bytesVal, kindStr, fanoutVal, fragVal)
            h = struct();
            h.from = char(fromRole);
            h.to = char(toRole);
            h.bytes = int64(max(0, round(double(bytesVal))));
            h.kind = char(kindStr);
            h.fanout = int64(max(0, round(double(fanoutVal))));
            h.fragmentsCount = int64(max(0, round(double(fragVal))));
        end

    end
end
