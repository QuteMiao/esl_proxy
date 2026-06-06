#!/usr/bin/env python3
"""
Generate HTML DAG visualization from pto._thread_0.csv
Extracts task dependencies and renders an interactive graph.
"""

import re
import csv
import json
from collections import defaultdict

def parse_csv(filepath):
    """Parse CSV and extract task dependencies."""
    edges = []  # (task_id, predecessor_id)
    
    with open(filepath, 'r') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header
        
        for row in reader:
            if len(row) < 2:
                continue
            detail = ','.join(row[2:])  # join detail fields
            
            if 'succeed' in detail:
                # Extract task_id and predecessor_id using regex
                task_match = re.search(r'task_id[,\s]+(\d+)', detail)
                pred_match = re.search(r'predecessor_id[,\s]+(\d+)', detail)
                
                if task_match and pred_match:
                    task_id = int(task_match.group(1))
                    pred_id = int(pred_match.group(1))
                    edges.append((task_id, pred_id))
    
    return edges

def build_dag(edges):
    """Build DAG structure from edges."""
    # Collect unique nodes and their predecessors
    nodes = set()
    predecessors = defaultdict(set)
    
    for task_id, pred_id in edges:
        nodes.add(task_id)
        nodes.add(pred_id)
        predecessors[task_id].add(pred_id)
    
    return nodes, predecessors

def generate_html(nodes, predecessors, output_path):
    """Generate interactive HTML DAG using D3.js."""
    
    # Prepare node data
    node_list = sorted(nodes)
    
    # Create edges data
    edge_list = []
    for task_id, preds in predecessors.items():
        for pred in preds:
            edge_list.append({
                'source': pred,
                'target': task_id,
            })
    
    # Build node data
    node_data = [{'id': n, 'label': f'T{n}'} for n in node_list]
    
    # Serialize to JSON for HTML
    nodes_json = json.dumps(node_data)
    links_json = json.dumps(edge_list)
    
    html = '''<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Task DAG Visualization</title>
    <script src="https://d3js.org/d3.v7.min.js"></script>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: Arial, sans-serif; 
            background: #1a1a2e;
            overflow: hidden;
        }}
        h1 {
            color: #eee;
            text-align: center;
            padding: 15px;
            margin: 0;
            background: #16213e;
        }
        #graph {
            width: 100vw;
            height: calc(100vh - 60px);
        }
        .node circle {
            stroke: #fff;
            stroke-width: 2px;
            cursor: pointer;
        }
        .node text {
            fill: #fff;
            font-size: 10px;
            pointer-events: none;
        }
        .link {
            stroke: #888;
            stroke-opacity: 0.6;
            fill: none;
            stroke-width: 1.5;
        }
        .tooltip {
            position: absolute;
            background: rgba(0,0,0,0.85);
            color: #fff;
            padding: 10px 15px;
            border-radius: 6px;
            font-size: 13px;
            pointer-events: none;
            z-index: 1000;
            border: 1px solid #555;
        }
        #stats {
            color: #aaa;
            text-align: center;
            padding: 5px;
            background: #16213e;
            font-size: 14px;
        }
    </style>
</head>
<body>
    <h1>Task Dependency DAG</h1>
    <div id="stats">Nodes: ''' + str(len(node_list)) + ''' | Edges: ''' + str(len(edge_list)) + ''' | Legend: Green=Root, Blue=Intermediate, Red=Leaf</div>
    <div id="graph"></div>
    <div id="tooltip" class="tooltip" style="display:none;"></div>
    
    <script>
        const nodes = ''' + nodes_json + ''';
        const links = ''' + links_json + ''';
        
        setTimeout(function() {
            const container = document.getElementById('graph');
            let width = container.clientWidth || window.innerWidth;
            let height = container.clientHeight || (window.innerHeight - 60);
            
            if (width === 0) width = 1200;
            if (height === 0) height = 800;
            
            const svg = d3.select('#graph')
                .append('svg')
                .attr('width', width)
                .attr('height', height)
                .attr('style', 'background: #0f0f23');
            
            const g = svg.append('g');
            
            // Arrow marker
            svg.append('defs').append('marker')
                .attr('id', 'arrowhead')
                .attr('viewBox', '-0 -5 10 10')
                .attr('refX', 25)
                .attr('refY', 0)
                .attr('orient', 'auto')
                .attr('markerWidth', 8)
                .attr('markerHeight', 8)
                .append('path')
                .attr('d', 'M 0,-5 L 10,0 L 0,5')
                .attr('fill', '#666');
            
            // Create link data with object references
            const linkData = links.map(function(l) {
                return {
                    source: nodes.find(function(n) { return n.id === l.source; }),
                    target: nodes.find(function(n) { return n.id === l.target; })
                };
            }).filter(function(l) { return l.source && l.target; });
            
            // Simulation
            const simulation = d3.forceSimulation(nodes)
                .force('link', d3.forceLink(linkData).id(function(d) { return d.id; }).distance(40).strength(0.5))
                .force('charge', d3.forceManyBody().strength(-50))
                .force('center', d3.forceCenter(width/2, height/2))
                .force('collision', d3.forceCollide().radius(15));
            
            // Links
            const link = g.append('g')
                .attr('class', 'links')
                .selectAll('line')
                .data(linkData)
                .enter().append('line')
                .attr('class', 'link')
                .attr('marker-end', 'url(#arrowhead)');
            
            // Nodes
            const node = g.append('g')
                .attr('class', 'nodes')
                .selectAll('g')
                .data(nodes)
                .enter().append('g')
                .attr('class', 'node')
                .call(d3.drag()
                    .on('start', dragstarted)
                    .on('drag', dragged)
                    .on('end', dragended));
            
            node.append('circle')
                .attr('r', 6)
                .attr('fill', function(d) {
                    const hasPreds = linkData.some(function(l) { return l.target.id === d.id; });
                    const hasSucc = linkData.some(function(l) { return l.source.id === d.id; });
                    if (!hasPreds) return '#4caf50';
                    if (!hasSucc) return '#f44336';
                    return '#2196f3';
                })
                .on('mouseover', showTooltip)
                .on('mouseout', hideTooltip);
            
            node.append('text')
                .attr('dx', 10)
                .attr('dy', 4)
                .text(function(d) { return d.label; });
            
            // Tooltip
            const tooltip = d3.select('#tooltip');
            
            function showTooltip(event, d) {
                const preds = linkData.filter(function(l) { return l.target.id === d.id; }).length;
                const succs = linkData.filter(function(l) { return l.source.id === d.id; }).length;
                tooltip.style('display', 'block')
                    .html('<strong>Task ' + d.id + '</strong><br>Predecessors: ' + preds + '<br>Dependents: ' + succs);
            }
            
            function hideTooltip() {
                tooltip.style('display', 'none');
            }
            
            simulation.on('tick', function() {
                link
                    .attr('x1', function(d) { return d.source.x; })
                    .attr('y1', function(d) { return d.source.y; })
                    .attr('x2', function(d) { return d.target.x; })
                    .attr('y2', function(d) { return d.target.y; });
                
                node.attr('transform', function(d) { return 'translate(' + d.x + ',' + d.y + ')'; });
            });
            
            function dragstarted(event) {
                if (!event.active) simulation.alphaTarget(0.3).restart();
                event.subject.fx = event.subject.x;
                event.subject.fy = event.subject.y;
            }
            
            function dragged(event) {
                event.subject.fx = event.x;
                event.subject.fy = event.y;
            }
            
            function dragended(event) {
                if (!event.active) simulation.alphaTarget(0);
                event.subject.fx = null;
                event.subject.fy = null;
            }
            
            // Update tooltip position and enable zoom
            const zoom = d3.zoom()
                .scaleExtent([0.1, 4])
                .on('zoom', function(event) {
                    g.attr('transform', event.transform);
                });
            
            svg.call(zoom);
            
            svg.on('mousemove', function(event) {
                tooltip.style('left', (event.pageX + 15) + 'px')
                       .style('top', (event.pageY - 10) + 'px');
            });
            
        }, 100);
    </script>
</body>
</html>'''
    
    with open(output_path, 'w') as f:
        f.write(html)
    
    print(f"DAG visualization saved to: {output_path}")
    print(f"Total nodes: {len(node_list)}, Total edges: {len(edge_list)}")

def main():
    import os
    
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    
    csv_path = os.path.join(project_root, 'esl_proxy', 'pto._thread_0.csv')
    output_path = os.path.join(project_root, 'report', 'dag.html')
    
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    
    print(f"Parsing: {csv_path}")
    edges = parse_csv(csv_path)
    
    print(f"Found {len(edges)} dependency edges")
    nodes, predecessors = build_dag(edges)
    
    print(f"Total unique nodes: {len(nodes)}")
    generate_html(nodes, predecessors, output_path)

if __name__ == '__main__':
    main()