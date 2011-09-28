// Copyright (c) 2006, 2007, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/
//
// $Id$

/// @file auto4_lua.cpp
/// @brief Lua 5.1-based scripting engine
/// @ingroup scripting
///

#include "config.h"

#ifdef WITH_AUTO4_LUA

#include "auto4_lua.h"

#ifndef AGI_PRE
#include <cassert>

#include <algorithm>

#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/tokenzr.h>
#include <wx/window.h>
#endif

#include <libaegisub/log.h>
#include <libaegisub/scoped_ptr.h>

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_override.h"
#include "ass_style.h"
#include "auto4_lua_factory.h"
#include "auto4_lua_scriptreader.h"
#include "main.h"
#include "standard_paths.h"
#include "text_file_reader.h"
#include "utils.h"
#include "video_context.h"

// This must be below the headers above.
#ifdef __WINDOWS__
#include "../../contrib/lua51/src/lualib.h"
#include "../../contrib/lua51/src/lauxlib.h"
#else
#include <lualib.h>
#include <lauxlib.h>
#endif

namespace {
	void push_value(lua_State *L, lua_CFunction fn) {
		lua_pushcfunction(L, fn);
	}

	void push_value(lua_State *L, int n) {
		lua_pushinteger(L, n);
	}

	void push_value(lua_State *L, double n) {
		lua_pushnumber(L, n);
	}

	template<class T>
	void set_field(lua_State *L, const char *name, T value) {
		push_value(L, value);
		lua_setfield(L, -2, name);
	}

	wxString get_global_string(lua_State *L, const char *name) {
		lua_getglobal(L, name);
		wxString ret;
		if (lua_isstring(L, -1))
			ret = wxString(lua_tostring(L, -1), wxConvUTF8);
		lua_pop(L, 1);
		return ret;
	}
}

namespace Automation4 {

	// LuaStackcheck
#if 0
	struct LuaStackcheck {
		lua_State *L;
		int startstack;
		void check_stack(int additional)
		{
			int top = lua_gettop(L);
			if (top - additional != startstack) {
				LOG_D("automation/lua") << "lua stack size mismatch.";
				dump();
				assert(top - additional == startstack);
			}
		}
		void dump()
		{
			int top = lua_gettop(L);
			LOG_D("automation/lua/stackdump") << "--- dumping lua stack...";
			for (int i = top; i > 0; i--) {
				lua_pushvalue(L, i);
				wxString type(lua_typename(L, lua_type(L, -1)), wxConvUTF8);
				if (lua_isstring(L, i)) {
					LOG_D("automation/lua/stackdump") << type << ": " << luatostring(L, -1);
				} else {
					LOG_D("automation/lua/stackdump") << type;
				}
				lua_pop(L, 1);
			}
			LOG_D("automation/lua") << "--- end dump";
		}
		LuaStackcheck(lua_State *_L) : L(_L) { startstack = lua_gettop(L); }
		~LuaStackcheck() { check_stack(0); }
	};
#else

	/// DOCME
	struct LuaStackcheck {

		/// @brief DOCME
		/// @param additional 
		///
		void check_stack(int additional) { }

		/// @brief DOCME
		///
		void dump() { }

		/// @brief DOCME
		/// @param L 
		///
		LuaStackcheck(lua_State *L) { }

		/// @brief DOCME
		///
		~LuaStackcheck() { }
	};
#endif


	// LuaScript
	LuaScript::LuaScript(wxString const& filename)
	: Script(filename)
	, L(0)
	{
		Create();
	}

	LuaScript::~LuaScript()
	{
		Destroy();
	}

	void LuaScript::Create()
	{
		Destroy();

		try {
			// create lua environment
			L = lua_open();
			LuaStackcheck _stackcheck(L);

			// register standard libs
			lua_pushcfunction(L, luaopen_base); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_package); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_string); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_table); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_math); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_io); lua_call(L, 0, 0);
			lua_pushcfunction(L, luaopen_os); lua_call(L, 0, 0);
			_stackcheck.check_stack(0);
			// dofile and loadfile are replaced with include
			lua_pushnil(L);
			lua_setglobal(L, "dofile");
			lua_pushnil(L);
			lua_setglobal(L, "loadfile");
			lua_pushcfunction(L, LuaInclude);
			lua_setglobal(L, "include");

			// add include_path to the module load path
			lua_getglobal(L, "package");
			lua_pushstring(L, "path");
			lua_pushstring(L, "path");
			lua_gettable(L, -3);

			wxStringTokenizer toker(lagi_wxString(OPT_GET("Path/Automation/Include")->GetString()), "|", wxTOKEN_STRTOK);
			while (toker.HasMoreTokens()) {
				wxFileName path(StandardPaths::DecodePath(toker.GetNextToken()));
				if (path.IsOk() && !path.IsRelative() && path.DirExists()) {
					wxCharBuffer p = path.GetLongPath().utf8_str();
					lua_pushfstring(L, ";%s?.lua;%s?/init.lua", p.data(), p.data());
					lua_concat(L, 2);
				}
			}

			lua_settable(L, -3);

			// Replace the default lua module loader with our unicode compatible one
			lua_getfield(L, -1, "loaders");
			lua_pushcfunction(L, LuaModuleLoader);
			lua_rawseti(L, -2, 2);
			lua_pop(L, 2);
			_stackcheck.check_stack(0);

			// prepare stuff in the registry
			// reference to the script object
			lua_pushlightuserdata(L, this);
			lua_setfield(L, LUA_REGISTRYINDEX, "aegisub");
			// the "feature" table
			// integer indexed, using same indexes as "features" vector in the base Script class
			lua_newtable(L);
			lua_setfield(L, LUA_REGISTRYINDEX, "features");
			_stackcheck.check_stack(0);

			// make "aegisub" table
			lua_pushstring(L, "aegisub");
			lua_newtable(L);

			set_field(L, "register_macro", LuaFeatureMacro::LuaRegister);
			set_field(L, "register_filter", LuaFeatureFilter::LuaRegister);
			set_field(L, "text_extents", LuaTextExtents);
			set_field(L, "frame_from_ms", LuaFrameFromMs);
			set_field(L, "ms_from_frame", LuaMsFromFrame);
			set_field(L, "video_size", LuaVideoSize);
			set_field(L, "lua_automation_version", 4);

			// store aegisub table to globals
			lua_settable(L, LUA_GLOBALSINDEX);
			_stackcheck.check_stack(0);

			// load user script
			LuaScriptReader script_reader(GetFilename());
			if (lua_load(L, script_reader.reader_func, &script_reader, GetPrettyFilename().utf8_str())) {
				wxString err(lua_tostring(L, -1), wxConvUTF8);
				err.Prepend("Error loading Lua script \"" + GetPrettyFilename() + "\":\n\n");
				throw ScriptLoadError(STD_STR(err));
			}
			_stackcheck.check_stack(1);

			// and execute it
			// this is where features are registered
			// don't thread this, as there's no point in it and it seems to break on wx 2.8.3, for some reason
			if (lua_pcall(L, 0, 0, 0)) {
				// error occurred, assumed to be on top of Lua stack
				wxString err(lua_tostring(L, -1), wxConvUTF8);
				err.Prepend("Error initialising Lua script \"" + GetPrettyFilename() + "\":\n\n");
				throw ScriptLoadError(STD_STR(err));
			}
			_stackcheck.check_stack(0);

			lua_getglobal(L, "version");
			if (lua_isnumber(L, -1) && lua_tointeger(L, -1) == 3) {
				lua_pop(L, 1); // just to avoid tripping the stackcheck in debug
				throw ScriptLoadError("Attempted to load an Automation 3 script as an Automation 4 Lua script. Automation 3 is no longer supported.");
			}

			name = get_global_string(L, "script_name");
			description = get_global_string(L, "script_description");
			author = get_global_string(L, "script_author");
			version = get_global_string(L, "script_version");

			if (name.empty())
				name = GetPrettyFilename();

			lua_pop(L, 1);
			// if we got this far, the script should be ready
			_stackcheck.check_stack(0);

		}
		catch (agi::Exception const& e) {
			Destroy();
			name = GetPrettyFilename();
			description = e.GetChainedMessage();
		}
	}

	void LuaScript::Destroy()
	{
		// Assume the script object is clean if there's no Lua state
		if (!L) return;

		delete_clear(features);

		lua_close(L);
		L = 0;
	}

	void LuaScript::Reload()
	{
		Create();
	}

	LuaScript* LuaScript::GetScriptObject(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, "aegisub");
		void *ptr = lua_touserdata(L, -1);
		lua_pop(L, 1);
		return (LuaScript*)ptr;
	}

	int LuaScript::RegisterFeature(Feature *feature) {
		features.push_back(feature);
		return features.size() - 1;
	}

	int LuaScript::LuaTextExtents(lua_State *L)
	{
		if (!lua_istable(L, 1))
			return luaL_error(L, "First argument to text_extents must be a table");

		if (!lua_isstring(L, 2))
			return luaL_error(L, "Second argument to text_extents must be a string");

		lua_pushvalue(L, 1);
		agi::scoped_ptr<AssEntry> et(LuaAssFile::LuaToAssEntry(L));
		AssStyle *st = dynamic_cast<AssStyle*>(et.get());
		lua_pop(L, 1);
		if (!st)
			return luaL_error(L, "Not a style entry");

		wxString text(lua_tostring(L, 2), wxConvUTF8);

		double width, height, descent, extlead;
		if (!CalculateTextExtents(st, text, width, height, descent, extlead))
			return luaL_error(L, "Some internal error occurred calculating text_extents");

		lua_pushnumber(L, width);
		lua_pushnumber(L, height);
		lua_pushnumber(L, descent);
		lua_pushnumber(L, extlead);
		return 4;
	}

	/// @brief Module loader which uses our include rather than Lua's, for unicode file support
	/// @param L The Lua state
	/// @return Always 1 per loader_Lua?
	int LuaScript::LuaModuleLoader(lua_State *L)
	{
		int pretop = lua_gettop(L);
		wxString module(lua_tostring(L, -1), wxConvUTF8);
		module.Replace(".", LUA_DIRSEP);

		lua_getglobal(L, "package");
		lua_pushstring(L, "path");
		lua_gettable(L, -2);
		wxString package_paths(lua_tostring(L, -1), wxConvUTF8);
		lua_pop(L, 2);

		wxStringTokenizer toker(package_paths, ";", wxTOKEN_STRTOK);
		while (toker.HasMoreTokens()) {
			wxString filename = toker.GetNextToken();
			filename.Replace("?", module);
			if (wxFileName::FileExists(filename)) {
				LuaScriptReader script_reader(filename);
				if (lua_load(L, script_reader.reader_func, &script_reader, filename.utf8_str())) {
					return luaL_error(L, "Error loading Lua module \"%s\":\n\n%s", filename.utf8_str().data(), lua_tostring(L, -1));
				}
			}
		}
		return lua_gettop(L) - pretop;
	}

	int LuaScript::LuaInclude(lua_State *L)
	{
		LuaScript *s = GetScriptObject(L);

		if (!lua_isstring(L, 1))
			return luaL_error(L, "Argument to include must be a string");

		wxString fnames(lua_tostring(L, 1), wxConvUTF8);

		wxFileName fname(fnames);
		if (fname.GetDirCount() == 0) {
			// filename only
			fname = s->include_path.FindAbsoluteValidPath(fnames);
		} else if (fname.IsRelative()) {
			// relative path
			wxFileName sfname(s->GetFilename());
			fname.MakeAbsolute(sfname.GetPath(true));
		} else {
			// absolute path, do nothing
		}
		if (!fname.IsOk() || !fname.FileExists())
			return luaL_error(L, "Lua include not found: %s", fnames.utf8_str().data());

		LuaScriptReader script_reader(fname.GetFullPath());
		if (lua_load(L, script_reader.reader_func, &script_reader, fname.GetFullName().utf8_str()))
			return luaL_error(L, "Error loading Lua include \"%s\":\n\n%s", fname.GetFullPath().utf8_str().data(), lua_tostring(L, -1));

		int pretop = lua_gettop(L) - 1; // don't count the function value itself
		lua_call(L, 0, LUA_MULTRET);
		return lua_gettop(L) - pretop;
	}

	int LuaScript::LuaFrameFromMs(lua_State *L)
	{
		int ms = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (VideoContext::Get()->TimecodesLoaded())
			lua_pushnumber(L, VideoContext::Get()->FrameAtTime(ms, agi::vfr::START));
		else
			lua_pushnil(L);

		return 1;
	}

	int LuaScript::LuaMsFromFrame(lua_State *L)
	{
		int frame = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (VideoContext::Get()->TimecodesLoaded())
			lua_pushnumber(L, VideoContext::Get()->TimeAtFrame(frame, agi::vfr::START));
		else
			lua_pushnil(L);

		return 1;
	}

	int LuaScript::LuaVideoSize(lua_State *L)
	{
		VideoContext *ctx = VideoContext::Get();
		if (ctx->IsLoaded()) {
			lua_pushnumber(L, ctx->GetWidth());
			lua_pushnumber(L, ctx->GetHeight());
			lua_pushnumber(L, ctx->GetAspectRatioValue());
			lua_pushnumber(L, ctx->GetAspectRatioType());
			return 4;
		} else {
			lua_pushnil(L);
			return 1;
		}
	}

	static void lua_threaded_call(ProgressSink *ps, lua_State *L, int nargs, int nresults, bool can_open_config)
	{
		LuaProgressSink lps(L, ps, can_open_config);

		if (lua_pcall(L, nargs, nresults, 0)) {
			// if the call failed, log the error here
			ps->Log("\n\nLua reported a runtime error:\n");
			ps->Log(lua_tostring(L, -1));
			lua_pop(L, 1);
		}

		lua_gc(L, LUA_GCCOLLECT, 0);
	}


	// LuaThreadedCall
	void LuaThreadedCall(lua_State *L, int nargs, int nresults, wxString const& title, wxWindow *parent, bool can_open_config)
	{
		BackgroundScriptRunner bsr(parent, title);
		try {
			bsr.Run(bind(lua_threaded_call, std::tr1::placeholders::_1, L, nargs, nresults, can_open_config));
		}
		catch (agi::UserCancelException const&) {
			/// @todo perhaps this needs to continue up for exporting?
		}
	}


	// LuaFeature


	/// @brief DOCME
	/// @param _L            
	/// @param _featureclass 
	/// @param _name         
	///
	LuaFeature::LuaFeature(lua_State *_L, ScriptFeatureClass _featureclass, const wxString &_name)
		: Feature(_featureclass, _name)
		, L(_L)
	{
	}


	/// @brief DOCME
	///
	void LuaFeature::RegisterFeature()
	{
		// get the LuaScript objects
		lua_getfield(L, LUA_REGISTRYINDEX, "aegisub");
		LuaScript *s = (LuaScript*)lua_touserdata(L, -1);
		lua_pop(L, 1);

		// add the Feature object
		myid = s->RegisterFeature(this);

		// create table with the functions
		// get features table
		lua_getfield(L, LUA_REGISTRYINDEX, "features");
		lua_pushvalue(L, -2);
		lua_rawseti(L, -2, myid);
		lua_pop(L, 1);
	}


	/// @brief DOCME
	/// @param functionid 
	///
	void LuaFeature::GetFeatureFunction(int functionid)
	{
		// get feature table
		lua_getfield(L, LUA_REGISTRYINDEX, "features");
		// get this feature's function pointers
		lua_rawgeti(L, -1, myid);
		// get pointer for validation function
		lua_rawgeti(L, -1, functionid);
		lua_remove(L, -2);
		lua_remove(L, -2);
	}


	/// @brief DOCME
	/// @param ints 
	///
	void LuaFeature::CreateIntegerArray(const std::vector<int> &ints)
	{
		// create an array-style table with an integer vector in it
		// leave the new table on top of the stack
		lua_newtable(L);
		for (size_t i = 0; i != ints.size(); ++i) {
			// We use zero-based indexing but Lua wants one-based, so add one
			lua_pushinteger(L, ints[i] + 1);
			lua_rawseti(L, -2, (int)i+1);
		}
	}


	/// @brief DOCME
	///
	void LuaFeature::ThrowError()
	{
		wxString err(lua_tostring(L, -1), wxConvUTF8);
		lua_pop(L, 1);
		wxLogError(err);
	}


	// LuaFeatureMacro


	/// @brief DOCME
	/// @param L 
	/// @return 
	///
	int LuaFeatureMacro::LuaRegister(lua_State *L)
	{
		wxString _name(lua_tostring(L, 1), wxConvUTF8);
		wxString _description(lua_tostring(L, 2), wxConvUTF8);

		LuaFeatureMacro *macro = new LuaFeatureMacro(_name, _description, L);
		(void)macro;

		return 0;
	}


	/// @brief DOCME
	/// @param _name        
	/// @param _description 
	/// @param _L           
	///
	LuaFeatureMacro::LuaFeatureMacro(const wxString &_name, const wxString &_description, lua_State *_L)
		: Feature(SCRIPTFEATURE_MACRO, _name)
		, FeatureMacro(_name, _description)
		, LuaFeature(_L, SCRIPTFEATURE_MACRO, _name)
	{
		// new table for containing the functions for this feature
		lua_newtable(L);
		// store processing function
		if (!lua_isfunction(L, 3)) {
			lua_pushstring(L, "The macro processing function must be a function");
			lua_error(L);
		}
		lua_pushvalue(L, 3);
		lua_rawseti(L, -2, 1);
		// and validation function
		lua_pushvalue(L, 4);
		no_validate = !lua_isfunction(L, -1);
		lua_rawseti(L, -2, 2);
		// make the feature known
		RegisterFeature();
		// and remove the feature function table again
		lua_pop(L, 1);
	}


	/// @brief DOCME
	/// @param subs     
	/// @param selected 
	/// @param active   
	/// @return 
	///
	bool LuaFeatureMacro::Validate(AssFile *subs, const std::vector<int> &selected, int active)
	{
		if (no_validate)
			return true;

		GetFeatureFunction(2);  // 2 = validation function

		// prepare function call
		LuaAssFile *subsobj = new LuaAssFile(L, subs, false, false);
		(void) subsobj;
		CreateIntegerArray(selected); // selected items
		lua_pushinteger(L, -1); // active line

		// do call
		int err = lua_pcall(L, 3, 1, 0);
		bool result;
		if (err) {
			wxString errmsg(lua_tostring(L, -1), wxConvUTF8);
			wxLogWarning("Runtime error in Lua macro validation function:\n%s", errmsg);
			result = false;
		} else {
			result = !!lua_toboolean(L, -1);
		}

		// clean up stack (result or error message)
		lua_pop(L, 1);

		return result;
	}


	/// @brief DOCME
	/// @param subs            
	/// @param selected        
	/// @param active          
	/// @param progress_parent 
	/// @return 
	///
	void LuaFeatureMacro::Process(AssFile *subs, std::vector<int> &selected, int active, wxWindow * const progress_parent)
	{
		GetFeatureFunction(1); // 1 = processing function
		LuaAssFile *subsobj = new LuaAssFile(L, subs, true, true);
		CreateIntegerArray(selected); // selected items
		lua_pushinteger(L, -1); // active line

		// do call
		// 3 args: subtitles, selected lines, active line
		// 1 result: new selected lines
		LuaThreadedCall(L, 3, 1, GetName(), progress_parent, true);

		subsobj->ProcessingComplete(GetName());

		// top of stack will be selected lines array, if any was returned
		if (lua_istable(L, -1)) {
			selected.clear();
			selected.reserve(lua_objlen(L, -1));
			lua_pushnil(L);
			while (lua_next(L, -2)) {
				if (lua_isnumber(L, -1)) {
					// Lua uses one-based indexing but we want zero-based, so subtract one
					selected.push_back(lua_tointeger(L, -1) - 1);
				}
				lua_pop(L, 1);
			}
			std::sort(selected.begin(), selected.end());
		}
		// either way, there will be something on the stack
		lua_pop(L, 1);
	}


	// LuaFeatureFilter


	/// @brief DOCME
	/// @param _name        
	/// @param _description 
	/// @param merit        
	/// @param _L           
	///
	LuaFeatureFilter::LuaFeatureFilter(const wxString &_name, const wxString &_description, int merit, lua_State *_L)
		: Feature(SCRIPTFEATURE_FILTER, _name)
		, FeatureFilter(_name, _description, merit)
		, LuaFeature(_L, SCRIPTFEATURE_FILTER, _name)
	{
		// Works the same as in LuaFeatureMacro
		lua_newtable(L);
		if (!lua_isfunction(L, 4)) {
			lua_pushstring(L, "The filter processing function must be a function");
			lua_error(L);
		}
		lua_pushvalue(L, 4);
		lua_rawseti(L, -2, 1);
		lua_pushvalue(L, 5);
		has_config = lua_isfunction(L, -1);
		lua_rawseti(L, -2, 2);
		RegisterFeature();
		lua_pop(L, 1);
	}


	/// @brief DOCME
	///
	void LuaFeatureFilter::Init()
	{
		// Don't think there's anything to do here... (empty in auto3)
	}


	/// @brief DOCME
	/// @param L 
	/// @return 
	///
	int LuaFeatureFilter::LuaRegister(lua_State *L)
	{
		wxString _name(lua_tostring(L, 1), wxConvUTF8);
		wxString _description(lua_tostring(L, 2), wxConvUTF8);
		int _merit = lua_tointeger(L, 3);

		LuaFeatureFilter *filter = new LuaFeatureFilter(_name, _description, _merit, L);
		(void) filter;

		return 0;
	}


	/// @brief DOCME
	/// @param subs          
	/// @param export_dialog 
	///
	void LuaFeatureFilter::ProcessSubs(AssFile *subs, wxWindow *export_dialog)
	{
		LuaStackcheck stackcheck(L);

		GetFeatureFunction(1); // 1 = processing function
		assert(lua_isfunction(L, -1));
		stackcheck.check_stack(1);

		// prepare function call
		// subtitles (undo doesn't make sense in exported subs, in fact it'll totally break the undo system)
		LuaAssFile *subsobj = new LuaAssFile(L, subs, true/*allow modifications*/, false/*disallow undo*/);
		assert(lua_isuserdata(L, -1));
		stackcheck.check_stack(2);
		// config
		if (has_config && config_dialog) {
			int results_produced = config_dialog->LuaReadBack(L);
			assert(results_produced == 1);
			(void) results_produced;	// avoid warning on release builds
			// TODO, write back stored options here
		} else {
			// no config so put an empty table instead
			lua_newtable(L);
		}
		assert(lua_istable(L, -1));
		stackcheck.check_stack(3);

		LuaThreadedCall(L, 2, 0, AssExportFilter::GetName(), export_dialog, false);

		stackcheck.check_stack(0);

		subsobj->ProcessingComplete();
	}


	/// @brief DOCME
	/// @param parent 
	/// @return 
	///
	ScriptConfigDialog* LuaFeatureFilter::GenerateConfigDialog(wxWindow *parent)
	{
		if (!has_config)
			return 0;

		GetFeatureFunction(2); // 2 = config dialog function

		// prepare function call
		// subtitles (don't allow any modifications during dialog creation, ideally the subs aren't even accessed)
		LuaAssFile *subsobj = new LuaAssFile(L, AssFile::top, false/*allow modifications*/, false/*disallow undo*/);
		(void) subsobj;
		// stored options
		lua_newtable(L); // TODO, nothing for now

		// do call
		int err = lua_pcall(L, 2, 1, 0);
		if (err) {
			wxString errmsg(lua_tostring(L, -1), wxConvUTF8);
			wxLogWarning("Runtime error in Lua macro validation function:\n%s", errmsg);
			lua_pop(L, 1); // remove error message
			return config_dialog = 0;
		} else {
			// Create config dialogue from table on top of stack
			return config_dialog = new LuaConfigDialog(L, false);
		}
	}

	LuaScriptFactory::LuaScriptFactory()
	: ScriptFactory("Lua", "*.lua")
	{
		Register(this);
	}

	Script* LuaScriptFactory::Produce(const wxString &filename) const
	{
		// Just check if file extension is .lua
		// Reject anything else
		if (filename.Right(4).Lower() == ".lua") {
			return new LuaScript(filename);
		} else {
			return 0;
		}
	}
};

#endif // WITH_AUTO4_LUA
