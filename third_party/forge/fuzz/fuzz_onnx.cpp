#include "fuzz.hpp"
#include <forge/import/onnx.hpp>
FORGE_FUZZ({
    auto m = forge::import::onnx_pb::parse_model({reinterpret_cast<const char*>(data), size});
    auto tr = forge::import::translate_onnx(m);
    (void)forge::modelfile::parse_blueprint(tr.blueprint);
})
