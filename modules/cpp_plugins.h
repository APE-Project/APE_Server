#ifndef _CPP_MODULES_APE_H
#define _CPP_MODULES_APE_H

#include "plugins.h"

#define APE_INIT_PLUGIN(modname, initfunc, modcallbacks) \
        extern "C" void ape_module_init(ace_plugins *module) \
        { \
                 infos_module.conf = NULL; \
                 module->cb = &modcallbacks; \
                 module->infos = &infos_module; \
                 module->loader = initfunc; \
                 module->modulename = modname; \
        }
#endif

