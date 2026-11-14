function summary = runHybridTraceExport(varargin)
%RUNHYBRIDTRACEEXPORT Export deterministic hybrid event traces for all 5 protocols.
%   Generates JSON traces conforming to swarm_trace_schema.json across
%   FlatSBP, HSBP, DTHSBP, LKH, and TGDH over the configured swarm sizes.
%
%   Usage:
%     runHybridTraceExport()
%     runHybridTraceExport('swarmSizes', [50 250 1000], 'simulationTime', 100)

    p = inputParser;
    addParameter(p, 'swarmSizes', [50, 100, 200, 250, 1000]);
    addParameter(p, 'protocols', {'FlatSBP', 'HSBP', 'DTHSBP', 'LKH', 'TGDH'});
    addParameter(p, 'seed', 42);
    addParameter(p, 'numClusters', 5);
    addParameter(p, 'simulationTime', 100);
    addParameter(p, 'lkhDegree', 3);
    addParameter(p, 'tgdhDegree', 2);
    parse(p, varargin{:});
    opts = p.Results;

    thisDir = fileparts(mfilename('fullpath'));
    matlabLayerDir = fileparts(thisDir);
    hybridRoot = fileparts(matlabLayerDir);
    addpath(genpath(matlabLayerDir));

    runsDir = fullfile(hybridRoot, 'traces', 'runs');
    samplesDir = fullfile(hybridRoot, 'traces', 'samples');
    if ~exist(runsDir, 'dir'), mkdir(runsDir); end
    if ~exist(samplesDir, 'dir'), mkdir(samplesDir); end

    records = struct('protocol', {}, 'N', {}, 'events', {}, 'bytes', {}, 'path', {});
    idx = 0;

    for sIdx = 1:numel(opts.swarmSizes)
        N = double(opts.swarmSizes(sIdx));
        for pIdx = 1:numel(opts.protocols)
            protoName = char(opts.protocols{pIdx});

            cfg = config();
            cfg.numUAVs = N;
            cfg.numClusters = double(opts.numClusters);
            cfg.clusterSize = round(N / cfg.numClusters);
            cfg.simulationTime = double(opts.simulationTime);
            cfg.randomSeed = double(opts.seed);
            cfg.enableTraceExport = true;

            rng(cfg.randomSeed, 'twister');
            swarm = Swarm(cfg);

            switch protoName
                case 'FlatSBP'
                    protocol = FlatSBP(swarm);
                case 'HSBP'
                    protocol = HSBP(swarm);
                case 'DTHSBP'
                    protocol = DTHSBP(swarm);
                case 'LKH'
                    protocol = TreeProtocolTraceAdapter(swarm, 'LKH', opts.lkhDegree);
                case 'TGDH'
                    protocol = TreeProtocolTraceAdapter(swarm, 'TGDH', opts.tgdhDegree);
                otherwise
                    error('runHybridTraceExport:UnknownProtocol', 'Unknown protocol: %s', protoName);
            end

            sim = Simulation(cfg, protocol);
            sim.run();

            prettyPrint = (N <= 250);
            outFile = fullfile(runsDir, sprintf('events_trace_%s_N%d.json', protoName, N));
            sim.exportTrace(outFile, prettyPrint);

            if N == 50
                sampleFile = fullfile(samplesDir, sprintf('events_trace_%s_N50.json', protoName));
                sim.exportTrace(sampleFile, true);
            end

            info = dir(outFile);
            idx = idx + 1;
            records(idx).protocol = protoName;
            records(idx).N = N;
            records(idx).events = numel(sim.traceLog);
            records(idx).bytes = info.bytes;
            records(idx).path = outFile;

            fprintf('[HybridTraceExport] proto=%-7s N=%-4d events=%-4d size=%d B\n', ...
                protoName, N, numel(sim.traceLog), info.bytes);
        end
    end

    summary = records;
end
