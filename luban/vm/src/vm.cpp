module luban.vm;

import std;
import luban.service;

namespace luban::vm {

int enter(const service::Context& c, std::string_view name_or_image, const EnterOptions&) {
    if (c.sink) c.sink(service::error(std::format("{}: machine mode is not built into this luban", name_or_image)));
    return 1;
}

}  // namespace luban::vm
