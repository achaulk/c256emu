#pragma once

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <lua.hpp>
#include <mutex>
#include <optional>
#include <vector>

#include "bus/int_controller.h"
#include "cpu/65816/cpu_65c816.h"
#include "debug_interface.h"

class System;

// Will be used for debugging / automation.
// Will be able to run Lua functions on breakpoints, or interactively from a
// REPL. Lua functions will include peek/poke, disassemble, register dump, set
// clear breakpoints, etc. Will be memory addressable as well, so that
// automation actions can be triggered from the running program.
class Automation {
 public:
  Automation(WDC65C816 *cpu, DebugInterface *debug_interface);
  ~Automation();

  // Called by the CPU before each instruction, when automation/debug mode is
  // on.
  bool Step();

  bool LoadScript(const std::string &path);
  bool Eval(const std::string &expression);

  void AddBreakpoint(uint32_t address, const std::string &function_name);
  void ClearBreakpoint(uint32_t address);

  struct Breakpoint {
    uint32_t address;
    std::string lua_function_name;
  };
  std::vector<Breakpoint> GetBreakpoints() const;

  System *system() { return system_; }

 private:
  void SetStopSteps(uint32_t steps);

  static int LuaStopCpu(lua_State *L);
  static int LuaContCpu(lua_State *L);
  static int LuaAddBreakpoint(lua_State *L);
  static int LuaClearBreakpoint(lua_State *L);
  static int LuaGetBreakpoints(lua_State *L);
  static int LuaGetCpuState(lua_State *L);
  static int LuaStep(lua_State *L);
  static int LuaPeek(lua_State *L);
  static int LuaPoke(lua_State *L);
  static int LuaPeek16(lua_State *L);
  static int LuaPoke16(lua_State *L);
  static int LuaPeekBuf(lua_State *L);
  static int LuaLoadHex(lua_State *L);
  static int LuaLoadBin(lua_State *L);
  static int LuaSys(lua_State *L);
  static int LuaTraceLog(lua_State *L);

  static const ::luaL_Reg c256emu_methods[];

  std::recursive_mutex lua_mutex_;

  DebugInterface *debug_interface_;
  WDC65C816 *cpu_;
  System *system_;

  ::lua_State *lua_state_;

  std::vector<Breakpoint> breakpoints_;
  std::optional<uint32_t> stop_steps_;
};
