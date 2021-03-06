// Copyright (c) 2012, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "XEReader.h"
#include "BitManip.h"
#include "Core.h"
#include "Node.h"
#include "PortAliases.h"
#include "ScopedArray.h"
#include "SystemState.h"
#include "XE.h"
#include "XMLUtils.h"
#include <iostream>
#include <map>
#include <cstdlib>
#include <cerrno>

using namespace axe;

static const char configSchema[] = {
#include "ConfigSchema.inc"
};

static const char XNSchema[] = {
#include "XNSchema.inc"
};

static const char XNTransform[] = {
#include "XNTransform.inc"
  // Ensure null termination.
  , '\0'
};

static long readNumberAttribute(xmlNode *node, const char *name)
{
  xmlAttr *attr = findAttribute(node, name);
  assert(attr);
  errno = 0;
  long value = std::strtol((char*)attr->children->content, 0, 0);
  if (errno != 0) {
    std::cerr << "Invalid " << name << '"' << (char*)attr->children->content
    << "\"\n";
    exit(1);
  }
  return value;
}

static std::auto_ptr<Core>
createCoreFromConfig(xmlNode *config, bool tracing)
{
  uint32_t ram_size = RAM_SIZE;
  uint32_t ram_base = RAM_BASE;
  xmlNode *memoryController = findChild(config, "MemoryController");
  xmlNode *ram = findChild(memoryController, "Ram");
  ram_base = readNumberAttribute(ram, "base");
  ram_size = readNumberAttribute(ram, "size");
  if (!isPowerOf2(ram_size)) {
    std::cerr << "Error: ram size is not a power of two\n";
    std::exit(1);
  }
  if ((ram_base % ram_size) != 0) {
    std::cerr << "Error: ram base is not a multiple of ram size\n";
    std::exit(1);
  }
  std::auto_ptr<Core> core(new Core(ram_size, ram_base, tracing));
  core->setCoreNumber(readNumberAttribute(config, "number"));
  if (xmlAttr *codeReference = findAttribute(config, "codeReference")) {
    core->setCodeReference((char*)codeReference->children->content);
  }
  return core;
}

static std::auto_ptr<Node>
createNodeFromConfig(xmlNode *config,
                     std::map<long,Node*> &nodeNumberMap, bool tracing)
{
  long jtagID = readNumberAttribute(config, "jtagId");
  Node::Type nodeType;
  if (!Node::getTypeFromJtagID(jtagID, nodeType)) {
    std::cerr << "Unknown jtagId 0x" << std::hex << jtagID << std::dec << '\n';
    std::exit(1);
  }
  long numXLinks = 0;
  if (xmlNode *switchNode = findChild(config, "Switch")) {
    if (findAttribute(switchNode, "sLinks")) {
      numXLinks = readNumberAttribute(switchNode, "sLinks");
    }
  }
  std::auto_ptr<Node> node(new Node(nodeType, numXLinks));
  long nodeID = readNumberAttribute(config, "number");
  nodeNumberMap.insert(std::make_pair(nodeID, node.get()));
  for (xmlNode *child = config->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE ||
        strcmp("Processor", (char*)child->name) != 0)
      continue;
    node->addCore(createCoreFromConfig(child, tracing));
  }
  return node;
}

static bool parseXLinkEnd(xmlAttr *attr, long &node, long &xlink)
{
  const char *s = (char*)attr->children->content;
  errno = 0;
  char *endp;
  node = std::strtol(s, &endp, 0);
  if (errno != 0 || *endp != ',')
    return false;
  xlink = std::strtol(endp + 1, &endp, 0);
  if (errno != 0 || *endp != '\0')
    return false;
  return true;
}

static Node *
lookupNodeChecked(const std::map<long,Node*> &nodeNumberMap, unsigned nodeID)
{
  std::map<long,Node*>::const_iterator it = nodeNumberMap.find(nodeID);
  if (it == nodeNumberMap.end()) {
    std::cerr << "No node matching id " << nodeID << std::endl;
    std::exit(1);
  }
  return it->second;
}

static std::auto_ptr<SystemState>
createSystemFromConfig(const std::string &filename,
                       const XESector *configSector,
                       bool tracing)
{
  uint64_t length = configSector->getLength();
  const scoped_array<char> buf(new char[length + 1]);
  if (!configSector->getData(buf.get())) {
    std::cerr << "Error reading config from \"" << filename << "\"" << std::endl;
    std::exit(1);
  }
  if (length < 8) {
    std::cerr << "Error unexpected config config sector length" << std::endl;
    std::exit(1);
  }
  length -= 8;
  buf[length] = '\0';
  
  xmlDoc *doc = xmlReadDoc(BAD_CAST buf.get(), "config.xml", NULL, 0);
  
  if (!checkDocAgainstSchema(doc, configSchema, sizeof(configSchema)))
    std::exit(1);
  
  xmlNode *root = xmlDocGetRootElement(doc);
  xmlNode *system = findChild(root, "System");
  xmlNode *nodes = findChild(system, "Nodes");
  std::auto_ptr<SystemState> systemState(new SystemState(tracing));
  std::map<long,Node*> nodeNumberMap;
  for (xmlNode *child = nodes->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE ||
        strcmp("Node", (char*)child->name) != 0)
      continue;
    systemState->addNode(createNodeFromConfig(child, nodeNumberMap, tracing));
  }
  xmlNode *connections = findChild(system, "Connections");
  for (xmlNode *child = connections->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE ||
        strcmp("SLink", (char*)child->name) != 0)
      continue;
    long nodeID1, link1, nodeID2, link2;
    if (!parseXLinkEnd(findAttribute(child, "end1"), nodeID1, link1)) {
      std::cerr << "Failed to parse \"end1\" attribute" << std::endl;
      std::exit(1);
    }
    if (!parseXLinkEnd(findAttribute(child, "end2"), nodeID2, link2)) {
      std::cerr << "Failed to parse \"end2\" attribute" << std::endl;
      std::exit(1);
    }
    Node *node1 = lookupNodeChecked(nodeNumberMap, nodeID1);
    if (link1 >= node1->getNumXLinks()) {
      std::cerr << "Invalid sLink number " << link1 << std::endl;
      std::exit(1);
    }
    Node *node2 = lookupNodeChecked(nodeNumberMap, nodeID2);
    if (link2 >= node2->getNumXLinks()) {
      std::cerr << "Invalid sLink number " << link2 << std::endl;
      std::exit(1);
    }
    node1->connectXLink(link1, node2, link2);
    node2->connectXLink(link2, node1, link1);
  }
  xmlNode *jtag = findChild(system, "JtagChain");
  unsigned jtagIndex = 0;
  for (xmlNode *child = jtag->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE ||
        strcmp("Node", (char*)child->name) != 0)
      continue;
    long nodeID = readNumberAttribute(child, "id");
    lookupNodeChecked(nodeNumberMap, nodeID)->setJtagIndex(jtagIndex++);
  }
  systemState->finalize();
  xmlFreeDoc(doc);
  return systemState;
}

std::auto_ptr<SystemState> XEReader::readConfig(bool tracing)
{
  // Load the file into memory.
  if (!xe) {
    std::cerr << "Error opening \"" << xe.getFileName() << "\"" << std::endl;
    std::exit(1);
  }
  // TODO handle XEs / XBs without a config sector.
  const XESector *configSector = xe.getConfigSector();
  if (!configSector) {
    std::cerr << "Error: No config file found in \"";
    std::cerr << xe.getFileName() << "\"" << std::endl;
    std::exit(1);
  }
  std::auto_ptr<SystemState> system =
  createSystemFromConfig(xe.getFileName(), configSector, tracing);
  return system;
}


static void readPortAliasesFromCore(PortAliases &aliases, xmlNode *core)
{
  std::string reference((char*)findAttribute(core, "Reference")->children->content);
  
  for (xmlNode *port = core->children; port; port = port->next) {
    if (port->type != XML_ELEMENT_NODE ||
        strcmp("Port", (char*)port->name) != 0)
      continue;
    std::string location((char*)findAttribute(port, "Location")->children->content);
    std::string name((char*)findAttribute(port, "Name")->children->content);
    aliases.add(name, reference, location);
  }
}

static void readPortAliasesFromNodes(PortAliases &aliases, xmlNode *nodes)
{
  for (xmlNode *node = nodes->children; node; node = node->next) {
    if (node->type != XML_ELEMENT_NODE ||
        strcmp("Node", (char*)node->name) != 0)
      continue;
    for (xmlNode *core = node->children; core; core = core->next) {
      if (core->type != XML_ELEMENT_NODE ||
          strcmp("Tile", (char*)core->name) != 0)
        continue;
      readPortAliasesFromCore(aliases, core);
    }
  }
}

static void readPortAliases(PortAliases &aliases, xmlNode *root) {
  xmlNode *packages = findChild(root, "Packages");
  for (xmlNode *child = packages->children; child; child = child->next) {
    if (child->type != XML_ELEMENT_NODE ||
        strcmp("Package", (char*)child->name) != 0)
      continue;
    readPortAliasesFromNodes(aliases, findChild(child, "Nodes"));
  }
}

void XEReader::readPortAliases(PortAliases &aliases)
{
  const XESector *XNSector = xe.getXNSector();
  if (!XNSector)
    return;
  uint64_t length = XNSector->getLength();
  const scoped_array<char> buf(new char[length + 1]);
  if (!XNSector->getData(buf.get())) {
    std::cerr << "Error reading XN from \"" << xe.getFileName() << "\"";
    std::cerr << std::endl;
    std::exit(1);
  }
  if (length < 8) {
    std::cerr << "Error unexpected XN sector length" << std::endl;
    std::exit(1);
  }
  length -= 8;
  buf[length] = '\0';
  
  xmlDoc *doc = xmlReadDoc(BAD_CAST buf.get(), "platform_def.xn", NULL, 0);
  xmlDoc *newDoc = applyXSLTTransform(doc, XNTransform);
  xmlFreeDoc(doc);
  if (!checkDocAgainstSchema(newDoc, XNSchema, sizeof(XNSchema)))
    std::exit(1);
  
  ::readPortAliases(aliases, xmlDocGetRootElement(newDoc));
  xmlFreeDoc(newDoc);
}
