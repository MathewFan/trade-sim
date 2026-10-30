"""Repeat the C++ benchmark and retain scope, environment, and a source fingerprint."""
import argparse
import datetime
import hashlib
import json
import platform
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', default='build/benchmark')
    parser.add_argument('--orders', type=int, default=1000000)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--output', default='benchmark-results.json')
    args = parser.parse_args()
    if not 128 <= args.orders <= 1000000 or not 1 <= args.trials <= 10:
        parser.error('orders must be 128..1000000; trials must be 1..10')
    root = Path(__file__).resolve().parents[1]
    fingerprint = hashlib.sha256()
    for path in sorted([root/'CMakeLists.txt', *root.glob('engine/*.*'), *root.glob('apps/*.cpp')]):
        fingerprint.update(path.relative_to(root).as_posix().encode()+b'\0'+path.read_bytes())
    results = []
    for trial in range(args.trials):
        print(f'Benchmark trial {trial + 1}/{args.trials}', flush=True)
        result = subprocess.run([str(Path(args.binary).resolve()), str(args.orders)], check=True, capture_output=True, text=True, timeout=120)
        results.append(json.loads(result.stdout))
    summaries = []
    for index, scenario in enumerate(results[0]['scenarios']):
        scenarios = [trial['scenarios'][index] for trial in results]
        summaries.append({'mode':scenario['mode'], 'shards':scenario['shards'], 'producers':scenario['producers'], 'commands_per_trial':scenario['commands'],
                          'median_commands_per_second':statistics.median(row['commands_per_second'] for row in scenarios),
                          'min_commands_per_second':min(row['commands_per_second'] for row in scenarios),
                          'max_commands_per_second':max(row['commands_per_second'] for row in scenarios),
                          'median_p99_observed_completion_us':statistics.median(row['observed_completion_us']['p99'] for row in scenarios)})
    report = {'recorded_at':datetime.datetime.now(datetime.timezone.utc).isoformat(), 'platform':platform.platform(),
              'source_sha256':fingerprint.hexdigest(), 'scope':'Synthetic in-process closed-loop benchmark; integer unit orders across 64 symbols, 64 outstanding per producer; no network, observer callbacks, replication, or production traffic.',
              'summaries':summaries, 'trials':results}
    Path(args.output).write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(summaries, indent=2))


if __name__ == '__main__':
    main()
