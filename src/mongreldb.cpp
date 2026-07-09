// src/mongreldb.cpp - compiled translation unit for the header-only client.
//
// The MongrelDB C++ client is header-only: the full implementation lives in
// include/mongreldb/mongreldb.hpp (which includes mongreldb_impl.hpp at the
// bottom). Most users should just include the header in their own TU:
//
//     #include <mongreldb/mongreldb.hpp>
//
// This file exists so that (a) the include path and libcurl link are exercised
// by the build, and (b) users who prefer a compiled TU can link it instead of
// relying on the header being included somewhere. Including the header here is
// sufficient: the templated/inline symbols are emitted into this TU.
//
// Licensing: MIT OR Apache-2.0.
// SPDX-License-Identifier: MIT OR Apache-2.0

#include "mongreldb/mongreldb.hpp"

// Nothing else to define: everything is inline.
