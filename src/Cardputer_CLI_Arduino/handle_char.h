#pragma once
#include <M5Cardputer.h>
#include <libssh/libssh.h>

extern ssh_channel g_channel;

void handleKbdChar(const Keyboard_Class::KeysState &st);
void handleSshChar(char c);

void termPrint(const String &s);
void termPrintln(const String &s);
void termClear();
