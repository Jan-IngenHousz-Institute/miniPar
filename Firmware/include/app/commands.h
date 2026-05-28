
#pragma once

#include <Arduino.h>

bool handleCommandText(const String &cmd, bool jsonMode = false);
bool handleJsonProtocol(const String &json);
