classdef TreeProtocolTraceAdapter < Protocol
    %TREEPROTOCOLTRACEADAPTER Trace-export wrapper for LKH and TGDH.
    %   Bridges static tree-key protocols (LKH(swarm,3) and TGDH(swarm,2))
    %   into Simulation.m's DES event loop while preserving exact RNG stream
    %   parity with HSBP/FlatSBP for paired trace generation.

    properties
        inner
        protocolName = ""
        treeDegree = 3
    end

    methods
        function obj = TreeProtocolTraceAdapter(swarm, protoName, degree)
            obj@Protocol(swarm);
            obj.protocolName = string(protoName);

            sInit = rng;
            restoreInit = onCleanup(@() rng(sInit)); %#ok<NASGU>

            switch char(obj.protocolName)
                case 'LKH'
                    if nargin < 3 || isempty(degree)
                        degree = 3;
                    end
                    obj.treeDegree = double(degree);
                    obj.inner = LKH(swarm, obj.treeDegree);
                case 'TGDH'
                    if nargin < 3 || isempty(degree)
                        degree = 2;
                    end
                    obj.treeDegree = double(degree);
                    obj.inner = TGDH(swarm, obj.treeDegree);
                otherwise
                    error('TreeProtocolTraceAdapter:UnsupportedProtocol', ...
                        'Expected LKH or TGDH, got %s', char(obj.protocolName));
            end
        end

        function initialize(obj)
            sBefore = rng;
            restoreRng = onCleanup(@() rng(sBefore)); %#ok<NASGU>
            obj.inner.initialize();
        end

        function result = join(obj, uavID, clusterID)
            % Advance RNG strictly by one UAV(...) construction (matching
            % HSBP/FlatSBP) and discard internal LKH/TGDH keygen randi draws.
            sBefore = rng;
            dummyUAV = UAV(uavID, clusterID, obj.cfg); %#ok<NASGU>
            sAfterUAV = rng;
            rng(sBefore);

            result = obj.inner.join(uavID, clusterID);
            rng(sAfterUAV);
        end

        function result = leave(obj, uavID)
            sBefore = rng;
            result = obj.inner.leave(uavID);
            rng(sBefore);
        end

        function result = failure(obj, uavID)
            sBefore = rng;
            result = obj.inner.failure(uavID);
            rng(sBefore);
        end
    end
end
