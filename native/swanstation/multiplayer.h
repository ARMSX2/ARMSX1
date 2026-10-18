// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace SwanMultiplayer {
inline unsigned FromPorts(bool port1,bool port2){return unsigned(port1)|(unsigned(port2)<<1);}
inline const char* CoreMode(unsigned mode) {
  switch(mode){case 1:return "Port1Only";case 2:return "Port2Only";case 3:return "BothPorts";default:return "Disabled";}
}
// Core logical ports run across port 1 then port 2. In port-2-only mode put
// players 1-4 on its A-D slots and leave the plain port 1 neutral.
inline int PlayerForPort(unsigned mode,unsigned port) {
  switch(mode) {
    case 1:return port<4?int(port):-1;
    case 2:return port>=1&&port<=4?int(port-1):-1;
    case 3:return port<8?int(port):-1;
    default:return port<2?int(port):-1;
  }
}
}
