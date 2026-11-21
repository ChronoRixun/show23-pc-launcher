#pragma once

#include <string>
#include <rapidjson/document.h>

namespace support
{
    void remember_display();
    void write_diagnostics(rapidjson::Document& out);
    bool copy_text(const std::string& text, void* owner_window);
}
