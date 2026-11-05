// Embedding of gui/style.css in the binary.
//
// The stylesheet is wrapped into a C string by the Makefile, so the window brings
// its own colours and does not depend on a file at run time nor on the theme of the
// desktop.
#pragma once

namespace fmiq {

// Dark stylesheet, loaded at USER priority when the window is built.
const char* dark_css();

}  // namespace fmiq
