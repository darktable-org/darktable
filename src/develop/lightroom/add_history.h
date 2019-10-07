#pragma once

#include <string>

extern "C"
{
#include "develop/develop.h"
}

namespace lightroom
{

// Add an iop to the development history
void add_history(int imgid, dt_develop_t const *dev, std::string const &operation_name, int version,
                 void const *params, int params_size);

}