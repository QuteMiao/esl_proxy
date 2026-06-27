#!/usr/bin/env python3
"""
Generate DAG visualization from pto._thread_0.csv
Extracts task dependencies and renders a graph.
"""

import re
import csv
import subprocess
from collections import defaultdict

# Task type mapping:0=CUBE, 1=VECTOR, 2=MIX
TASK_TYPE_NAMES = {
    0: 'CUBE',
    1: 'VECTOR',
    2: 'MIX',
}

def parse_csv(filepath):
    """Parse CSV and extract task dependencies and task types."""
    edges = []  # (task_id, predecessor_id)
    task_types = {}  # task_id -> type
    
    with open(filepath, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header
        
        for row in reader:
            if len(row) < 2:
                continue
            detail = ','.join(row[2:])  # join detail fields
            
            if 'new' in detail:
                # Extract task_id and type from enqueue records
                # Format: "enqueue task_id,2, type,1, ctrl_id,0, cnt,0"
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                type_match = re.search(r'type[,\s]+(\d+)', detail)
                
                if task_match and type_match:
                    task_id = int(task_match.group(1))
                    task_type = int(type_match.group(1))
                    task_types[task_id] = task_type
            
            elif 'succeed' in detail:
                # Extract task_id and predecessor using regex
                # Format: "succeed task_id,3,predecessor,2,target,2"
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                pred_match = re.search(r'predecessor_id[,\s]+(\d+)', detail)
                
                if task_match and pred_match:
                    task_id = int(task_match.group(1))
                    pred_id = int(pred_match.group(1))
                    edges.append((task_id, pred_id))
            elif 'cutter' in detail:
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                succ_match = re.search(r'successor_id[,\s]+(\d+)', detail)
                
                if task_match and succ_match:
                    task_id = int(task_match.group(1))
                    succ_id = int(succ_match.group(1))
                    edges.append((succ_id, task_id))
    return edges, task_types


def generate_dot(task_types, edges, output_path):
    """Generate DOT file for DAG visualization.
    
    Args:
        task_types: List/array where index is node id and value is task type
        edges: List of (task_id, predecessor_id) tuples representing dependencies
        output_path: Path to save the DOT file
    """
    # Build predecessor mapping from edges
    predecessors = defaultdict(list)
    for task_id, pred_id in edges:
        predecessors[task_id].append(pred_id)
    
    # Get all node IDs from task_types
    nodes = set()
    for node_id, task_type in enumerate(task_types):
        if node_id in predecessors or any(pred in task_types for pred in predecessors.get(node_id, [])):
            nodes.add(node_id)
        elif task_type is not None:
            nodes.add(node_id)
    
    # If nodes is empty, try to get nodes from task_types dict
    if not nodes and isinstance(task_types, dict):
        nodes = set(task_types.keys())
    
    # Build DOT content with white background and vertical layout
    dot_lines = [
        'digraph DAG {',
        '  rankdir=TB;',  # Top-to-bottom vertical layout
        '  bgcolor="#ffffff";',
        '  node [fontname="Arial" fontcolor="#333"];',
        '  edge [color="#666" fontname="Arial"];',
        '',
    ]
    
    # Add nodes with black font color
    if isinstance(task_types, (list, tuple)):
        for node_id, task_type in enumerate(task_types):
            if task_type is not None:
                type_name = TASK_TYPE_NAMES.get(task_type, 'UNKNOWN')
                shape_map = {0: 'rect', 1: 'ellipse', 2: 'diamond'}
                shape = shape_map.get(task_type, 'rect')
                color_map = {0: '#ff9800', 1: '#9c27b0', 2: '#00bcd4'}
                color = color_map.get(task_type, '#ff9800')
                dot_lines.append(f'  T{node_id} [label="T{node_id}\\n{type_name}" shape={shape} fillcolor="{color}" fontcolor="#000000"];')
    elif isinstance(task_types, dict):
        for node_id in sorted(task_types.keys()):
            task_type = task_types[node_id]
            type_name = TASK_TYPE_NAMES.get(task_type, 'UNKNOWN')
            shape_map = {0: 'rect', 1: 'ellipse', 2: 'diamond'}
            shape = shape_map.get(task_type, 'rect')
            color_map = {0: '#ff9800', 1: '#9c27b0', 2: '#00bcd4'}
            color = color_map.get(task_type, '#ff9800')
            dot_lines.append(f'  T{node_id} [label="T{node_id}\\n{type_name}" shape={shape} fillcolor="{color}" fontcolor="#000000"];')
    
    dot_lines.append('')
    
    # Add edges
    for task_id, pred_id in sorted(edges):
        dot_lines.append(f'  T{pred_id} -> T{task_id};')
    
    dot_lines.append('}')
    
    # Write to file
    with open(output_path, 'w') as f:
        f.write('\n'.join(dot_lines))
    
    print(f"DOT file saved to: {output_path}")


def render_dot(dot_path, output_path, format='svg'):
    """Render DOT file to image using Graphviz."""
    try:
        # Check if dot command is available
        result = subprocess.run(['which', 'dot'], capture_output=True, text=True)
        if result.returncode != 0:
            print("Warning: Graphviz 'dot' command not found. Install with: brew install graphviz")
            print(f"Generated DOT file: {dot_path}")
            return False
        
        # Render using dot
        cmd = ['dot', '-T', format, '-o', output_path, dot_path]
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode == 0:
            print(f"Rendered image saved to: {output_path}")
            return True
        else:
            print(f"Error rendering DOT: {result.stderr}")
            return False
            
    except Exception as e:
        print(f"Error rendering DOT: {e}")
        return False

def analyze(task_types, edges):
    """Print DAG shape statistics."""
    from collections import Counter, defaultdict, deque

    nodes = set(task_types.keys())
    for t, p in edges:
        nodes.add(t)
        nodes.add(p)
    indeg = Counter()
    outdeg = Counter()
    adj = defaultdict(list)
    for t, p in edges:
        adj[p].append(t)
        indeg[t] += 1
        outdeg[p] += 1
    tc = Counter(task_types.values())
    sources = [n for n in nodes if indeg[n] == 0]
    sinks = [n for n in nodes if outdeg[n] == 0]
    level = {}
    q = deque()
    for s in sources:
        level[s] = 0
        q.append(s)
    while q:
        u = q.popleft()
        for v in adj[u]:
            nv = level[u] + 1
            if v not in level or level[v] < nv:
                level[v] = nv
                q.append(v)
    depth = max(level.values()) + 1 if level else 0
    print(f"  tasks={len(task_types)}  edges={len(edges)}  depth={depth}")
    print(f"  types: CUBE={tc[0]} VECTOR={tc[1]} MIX={tc[2]}")
    print(f"  sources={len(sources)}  sinks={len(sinks)}  max_in={max(indeg.values()) if indeg else 0}  max_out={max(outdeg.values()) if outdeg else 0}")


def process_log(log_file, dot_file=None, svg_file=None):
    dot_file = dot_file or log_file.replace('.csv', '.dot')
    svg_file = svg_file or log_file.replace('.csv', '.svg')

    print(f"Parsing: {log_file}")
    edges, task_types = parse_csv(log_file)

    print(f"Found {len(edges)} dependency edges")
    print(f"Found {len(task_types)} task type mappings")

    if not edges:
        print("No dependency edges found in CSV")
        return False

    analyze(task_types, edges)
    generate_dot(task_types, edges, dot_file)
    render_dot(dot_file, svg_file, 'svg')
    return True


def main():
    import argparse
    import os

    parser = argparse.ArgumentParser(description='Generate DAG from pto._thread_*.csv')
    parser.add_argument('csv', nargs='*', help='CSV log file(s)')
    parser.add_argument('-o', '--out-dir', help='Output directory for .dot/.svg')
    args = parser.parse_args()

    if args.csv:
        log_files = args.csv
    else:
        default = os.path.join('esl_proxy', 'log', 'pto._thread_0.csv')
        log_files = [default]

    for log_file in log_files:
        if args.out_dir:
            base = os.path.splitext(os.path.basename(log_file))[0]
            dot_file = os.path.join(args.out_dir, base + '.dot')
            svg_file = os.path.join(args.out_dir, base + '.svg')
            os.makedirs(args.out_dir, exist_ok=True)
        else:
            dot_file = svg_file = None
        process_log(log_file, dot_file, svg_file)


if __name__ == '__main__':
    main()