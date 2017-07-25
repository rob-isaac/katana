#include <unordered_map>
#include <utility>

#include "Galois/Graphs/Graph.h"

#include "Verilog.h"
#include "Sdc.h"

#ifndef GALOIS_CIRCUIT_GRAPH_H
#define GALOIS_CIRCUIT_GRAPH_H

struct Node {
  VerilogPin *pin;
  float slew, totalNetC, totalPinC;
  float arrivalTime, requiredTime, slack;
  float internalPower, netPower;
  bool isRise;
  bool isDummy, isPrimary, isOutput;
  size_t precondition;
};

struct Edge {
  VerilogWire *wire;
  float delay;
};

typedef Galois::Graph::FirstGraph<Node, Edge, true, true> Graph;
typedef Graph::GraphNode GNode;

extern Graph graph;
extern std::unordered_map<VerilogPin *, GNode> nodeMap;
extern GNode dummySrc, dummySink;

void constructCircuitGraph(Graph& g, VerilogModule& vModule);
void initializeCircuitGraph(Graph& g, SDC& sdc);
void printCircuitGraph(Graph& g);
std::pair<size_t, size_t> getCircuitGraphStatistics(Graph& g);

#endif // GALOIS_CIRCUIT_GRAPH_H

