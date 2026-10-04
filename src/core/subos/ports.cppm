export module xlings.core.subos.ports;

import std;
import xlings.core.config;
import xlings.runtime;
import xlings.subos.home_view;
import xlings.subos.ports;

// The adapter between xlings core and the SubOS core (design §23): this home
// as the SubOS core's HomeView, and xlings' package manager behind its Ports.
// Everything that knows both sides lives in src/core/subos/; modules/subos
// knows neither Config nor xim.
export namespace xlings::subos {

// This home, as the SubOS core sees it.
HomeView home_view();

// The ports, bound to an event stream for anything they report.
Ports make_ports(EventStream& stream);

}  // namespace xlings::subos
