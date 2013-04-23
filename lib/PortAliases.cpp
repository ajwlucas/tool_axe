// Copyright (c) 2012, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#include "PortAliases.h"

using namespace axe;

bool PortAliases::
lookup(const std::string &name, std::string &core, std::string &port) const
{
  auto it = aliases.find(name);
  if (it == aliases.end())
    return false;
  core = it->second.first;
  port = it->second.second;
  return true;
}
