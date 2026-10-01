import subprocess
import signal
import time
import os
import csv

def run_profiler():
    binary_path = "./benchmark"
    csv_filename = "benchmark_perf.csv"
    
    # Events you want perf to track
    # events_L1_L2 = ['L1-dcache-loads', 'L1-dcache-load-misses', 'l2_rqsts.references', 'l2_rqsts.miss']
    # events_LLC = ['LLC-loads', 'LLC-load-misses', 'LLC-stores', 'LLC-store-misses']
    events = [
        'L1-dcache-load-misses',
        'LLC-load-misses',
        'page-faults'
    ]

    print("[Python] Starting C benchmark process...")
    
    # Start the C binary
    # bufsize=1 ensures line-buffered output so we get the 'tid' immediately
    p = subprocess.Popen(
        [binary_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    tid = None
    
    # Parse stdout to find the worker thread ID
    for line in p.stdout:
        if line.startswith("tid"):
            tid = line.strip().split(" ")[1]
            break

    if not tid:
        print("[Python] Failed to capture Thread ID. Exiting.")
        p.kill()
        return

    print(f"[Python] Captured Worker Thread ID: {tid}")
    print("[Python] Attaching perf stat to thread...")

    # Attach perf specifically to the isolated C thread
    perf_cmd = [
        'perf', 'stat',
        '-t', str(tid),
        '-x,',  # Output as CSV
        '-e', ','.join(events)
    ]

    perf_proc = subprocess.Popen(
        perf_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    # Give perf a fraction of a second to fully attach to the OS thread
    time.sleep(0.05)

    print("[Python] Unblocking C execution loop via stdin...")
    # Trigger the C program to start the actual workload
    p.stdin.write("\n")
    p.stdin.flush()

    # Wait for the C program's timer to finish
    p.wait()
    print("[Python] C process finished.")

    # Kill perf to force it to dump its metrics
    perf_proc.send_signal(signal.SIGINT)
    _, perf_stderr = perf_proc.communicate()

    # Parse perf output
    perf_data = {}
    for line in perf_stderr.splitlines():
        parts = line.strip().split(',')
        if len(parts) >= 3:
            value = parts[0].strip()
            event_name = parts[2].strip().split(':')[0]
            if event_name in events:
                perf_data[event_name] = value

    # Write external perf results to CSV
    file_exists = os.path.isfile(csv_filename)
    with open(csv_filename, mode='a', newline='') as f:
        writer = csv.writer(f)
        if not file_exists:
            writer.writerow(events)
        
        row = [perf_data.get(e, "0") for e in events]
        writer.writerow(row)
        
    print(f"[Python] Results saved to {csv_filename}")
    for k, v in perf_data.items():
        print(f"  {k}: {v}")

if __name__ == "__main__":
    run_profiler()