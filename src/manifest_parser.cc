// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "manifest_parser.h"
#include "disk_interface.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "graph.h"
#include "state.h"
#include "util.h"
#include "version.h"

ManifestParser::ManifestParser(State* state, FileReader* file_reader,
                               ManifestParserOptions options)
    : Parser(state, file_reader),
      options_(options), quiet_(false) {
  env_ = &state->bindings_;
}

bool ManifestParser::Parse(const string& filename, const string& input,
                           string* err) {
  lexer_.Start(filename, input);

  for (;;) {
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
    case Lexer::POOL:
      if (!ParsePool(err))
        return false;
      break;
    case Lexer::BUILD:
      if (!ParseEdge(err))
        return false;
      break;
    case Lexer::RULE:
      if (!ParseRule(err))
        return false;
      break;
    case Lexer::DEFAULT:
      if (!ParseDefault(err))
        return false;
      break;
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      string name;
      EvalString let_value;
      if (!ParseLet(&name, &let_value, err))
        return false;
      string value = let_value.Evaluate(env_);
      // Check ninja_required_version immediately so we can exit
      // before encountering any syntactic surprises.
      if (name == "ninja_required_version")
        CheckNinjaVersion(value);
      env_->AddBinding(name, value);
      break;
    }
    case Lexer::INCLUDE:
      if (!ParseFileInclude(false, err))
        return false;
      break;
    case Lexer::SUBNINJA:
      if (!ParseFileInclude(true, err))
        return false;
      break;
    case Lexer::ERROR: {
      return lexer_.Error(lexer_.DescribeLastError(), err);
    }
    case Lexer::TEOF:
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}


bool ManifestParser::ParsePool(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected pool name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (state_->LookupPool(name) != NULL)
    return lexer_.Error("duplicate pool '" + name + "'", err);

  int depth = -1;

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (key == "depth") {
      string depth_string = value.Evaluate(env_);
      depth = atol(depth_string.c_str());
      if (depth < 0)
        return lexer_.Error("invalid pool depth", err);
    } else {
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (depth < 0)
    return lexer_.Error("expected 'depth =' line", err);

  state_->AddPool(new Pool(name, depth));
  return true;
}


bool ManifestParser::ParseRule(string* err) {
  string name;
  if (!lexer_.ReadIdent(&name))
    return lexer_.Error("expected rule name", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (env_->LookupRuleCurrentScope(name) != NULL)
    return lexer_.Error("duplicate rule '" + name + "'", err);

  Rule* rule = new Rule(name);  // XXX scoped_ptr

  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString value;
    if (!ParseLet(&key, &value, err))
      return false;

    if (Rule::IsReservedBinding(key)) {
      rule->AddBinding(key, value);
    } else {
      // Die on other keyvals for now; revisit if we want to add a
      // scope here.
      return lexer_.Error("unexpected variable '" + key + "'", err);
    }
  }

  if (rule->bindings_["rspfile"].empty() !=
      rule->bindings_["rspfile_content"].empty()) {
    return lexer_.Error("rspfile and rspfile_content need to be "
                        "both specified", err);
  }

  if (rule->bindings_["command"].empty())
    return lexer_.Error("expected 'command =' line", err);

  env_->AddRule(rule);
  return true;
}

bool ManifestParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  if (!ExpectToken(Lexer::EQUALS, err))
    return false;
  if (!lexer_.ReadVarValue(value, err))
    return false;
  return true;
}

bool ManifestParser::ParseDefault(string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  if (eval.empty())
    return lexer_.Error("expected target name", err);

  do {
    string path = eval.Evaluate(env_);
    string path_err;
    uint64_t slash_bits;  // Unused because this only does lookup.
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddDefault(path, env_, &path_err))
      return lexer_.Error(path_err, err);

    eval.Clear();
    if (!lexer_.ReadPath(&eval, err))
      return false;
  } while (!eval.empty());

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  return true;
}

bool ManifestParser::ParseEdge(string* err) {
  vector<EvalString> ins, outs;

  {
    EvalString out;
    if (!lexer_.ReadPath(&out, err))
      return false;
    while (!out.empty()) {
      outs.push_back(out);

      out.Clear();
      if (!lexer_.ReadPath(&out, err))
        return false;
    }
  }

  // Add all implicit outs, counting how many as we go.
  int implicit_outs = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString out;
      if (!lexer_.ReadPath(&out, err))
        return err;
      if (out.empty())
        break;
      outs.push_back(out);
      ++implicit_outs;
    }
  }

  if (outs.empty())
    return lexer_.Error("expected path", err);

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return lexer_.Error("expected build command name", err);

  const Rule* rule = env_->LookupRule(rule_name);
  if (!rule)
    return lexer_.Error("unknown build rule '" + rule_name + "'", err);

  for (;;) {
    // XXX should we require one path here?
    EvalString in;
    if (!lexer_.ReadPath(&in, err))
      return false;
    if (in.empty())
      break;
    ins.push_back(in);
  }

  // Add all implicit deps, counting how many as we go.
  int implicit = 0;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString in;
      if (!lexer_.ReadPath(&in, err))
        return err;
      if (in.empty())
        break;
      ins.push_back(in);
      ++implicit;
    }
  }

  // Add all order-only deps, counting how many as we go.
  int order_only = 0;
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    for (;;) {
      EvalString in;
      if (!lexer_.ReadPath(&in, err))
        return false;
      if (in.empty())
        break;
      ins.push_back(in);
      ++order_only;
    }
  }

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  // Bindings on edges are rare, so allocate per-edge envs only when needed.
  bool has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  BindingEnv* env = has_indent_token ? new BindingEnv(env_) : env_;
  while (has_indent_token) {
    string key;
    EvalString val;
    if (!ParseLet(&key, &val, err))
      return false;

    env->AddBinding(key, val.Evaluate(env_));
    has_indent_token = lexer_.PeekToken(Lexer::INDENT);
  }

  Edge* edge = state_->AddEdge(rule);
  edge->env_ = env;

  string pool_name = edge->GetBinding("pool");
  if (!pool_name.empty()) {
    Pool* pool = state_->LookupPool(pool_name);
    if (pool == NULL)
      return lexer_.Error("unknown pool name '" + pool_name + "'", err);
    edge->pool_ = pool;
  }

  edge->outputs_.reserve(outs.size());
  for (size_t i = 0, e = outs.size(); i != e; ++i) {
    string path = outs[i].Evaluate(env);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    if (!state_->AddOut(edge, path, slash_bits)) {
      if (options_.dupe_edge_action_ == kDupeEdgeActionError) {
        lexer_.Error("multiple rules generate " + path + " [-w dupbuild=err]",
                     err);
        return false;
      } else {
        if (!quiet_) {
          Warning("multiple rules generate %s. "
                  "builds involving this target will not be correct; "
                  "continuing anyway [-w dupbuild=warn]",
                  path.c_str());
        }
        if (e - i <= static_cast<size_t>(implicit_outs))
          --implicit_outs;
      }
    }
  }
  if (edge->outputs_.empty()) {
    // All outputs of the edge are already created by other edges. Don't add
    // this edge.  Do this check before input nodes are connected to the edge.
    state_->edges_.pop_back();
    delete edge;
    return true;
  }
  edge->implicit_outs_ = implicit_outs;

  edge->inputs_.reserve(ins.size());
  for (vector<EvalString>::iterator i = ins.begin(); i != ins.end(); ++i) {
    string path = i->Evaluate(env);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer_.Error(path_err, err);
    state_->AddIn(edge, path, slash_bits);
  }
  edge->implicit_deps_ = implicit;
  edge->order_only_deps_ = order_only;

  if (options_.phony_cycle_action_ == kPhonyCycleActionWarn &&
      edge->maybe_phonycycle_diagnostic()) {
    // CMake 2.8.12.x and 3.0.x incorrectly write phony build statements
    // that reference themselves.  Ninja used to tolerate these in the
    // build graph but that has since been fixed.  Filter them out to
    // support users of those old CMake versions.
    Node* out = edge->outputs_[0];
    vector<Node*>::iterator new_end =
        remove(edge->inputs_.begin(), edge->inputs_.end(), out);
    if (new_end != edge->inputs_.end()) {
      edge->inputs_.erase(new_end, edge->inputs_.end());
      if (!quiet_) {
        Warning("phony target '%s' names itself as an input; "
                "ignoring [-w phonycycle=warn]",
                out->path().c_str());
      }
    }
  }

  // Multiple outputs aren't (yet?) supported with depslog.
  string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty() && edge->outputs_.size() > 1) {
    return lexer_.Error("multiple outputs aren't (yet?) supported by depslog; "
                        "bring this up on the mailing list if it affects you",
                        err);
  }

  // Lookup, validate, and save any dyndep binding.  It will be used later
  // to load generated dependency information dynamically, but it must
  // be one of our manifest-specified inputs.
  string dyndep = edge->GetUnescapedDyndep();
  if (!dyndep.empty()) {
    dyndep = env->ApplyChdir(dyndep);
    uint64_t slash_bits;
    if (!CanonicalizePath(&dyndep, &slash_bits, err))
      return false;
    edge->dyndep_ = state_->GetNode(dyndep, env, slash_bits);
    edge->dyndep_->set_dyndep_pending(true);
    vector<Node*>::iterator dgi =
      std::find(edge->inputs_.begin(), edge->inputs_.end(), edge->dyndep_);
    if (dgi == edge->inputs_.end()) {
      return lexer_.Error("dyndep '" + dyndep + "' is not an input", err);
    }
  }

  return true;
}

bool ManifestParser::ParseFileInclude(bool new_scope, string* err) {
  EvalString eval;
  if (!lexer_.ReadPath(&eval, err))
    return false;
  string path = eval.Evaluate(env_);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  bool has_chdir = false;
  string parent_path = env_->AsString();
  string dir_err;
  string prev_cwd;
  string rel_path;
  bool ok = true;  // Always restore prev_cwd, even if there is an error.
  while (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString let_value;
    if (!ParseLet(&key, &let_value, err)) {
      ok = false;
      break;
    }
    if (key != "chdir") {
      lexer_.Error("illegal key '" + key + "' (only 'chdir' is supported)",
                   err);
      ok = false;
      break;
    }
    if (!new_scope) {
      lexer_.Error("invalid use of 'chdir' in include line", err);
      ok = false;
      break;
    }
    if (has_chdir) {
      lexer_.Error("duplicate 'chdir' in subninja", err);
      ok = false;
      break;
    }
    rel_path = let_value.Evaluate(env_);
    // if (rel_path.find("./") == 0 || rel_path.find("/./") != string::npos ||
    //     rel_path.find("../") == 0 || rel_path.find("/../") != string::npos) {
    //   return lexer_.Error("invalid use of '.' or '..' in chdir", err);
    // }
    if (file_reader_->Getcwd(&prev_cwd, &dir_err) != FileReader::Okay) {
      *err = "Getcwd: " + dir_err;
      return false;  // Can return because has_chdir is always false here.
    }
    if (file_reader_->Chdir(rel_path, &dir_err) != FileReader::Okay) {
      *err = "Chdir to '" + rel_path + "': " + dir_err;
      return false;
    }
    has_chdir = true;
    rel_path += '/';
  }

  if (ok) {
    ManifestParser subparser(state_, file_reader_, options_);
    if (new_scope) {
      // Note that rel_path of "" (for whatever reason) means no chdir.
      subparser.env_ = new BindingEnv(env_, rel_path,
                                      parent_path + rel_path);

      if (!rel_path.empty()) {
        subparser.env_->AddRule(&State::kPhonyRule);

        const string& abs_path = subparser.env_->AsString();
        // Find all inbound references to this chdir. Their env will be a parent
        // of subparser.env_ and assume no chdir. Replace it so that
        // subparser.env_ is used correctly with the chdir in place.
        for (State::Paths::iterator i = state_->paths_.begin();
            i != state_->paths_.end(); ++i) {
          if (i->first.AsString().find(abs_path) == 0) {
            Node* node = i->second;
            node->ResetEnv(subparser.env_);
          }
        }
      }
    } else {
      subparser.env_ = env_;
    }

    if (!subparser.Load(path, err, &lexer_)) {
      ok = false;
    }
  }

  // Restore prev_cwd. Note that has_chdir is set to true only after
  // file_reader_->Chdir() succeeds, and any errors after that set ok = false.
  // Do not smash the value of err if it was previously set.
  if (has_chdir) {
    if (file_reader_->Chdir(prev_cwd, &dir_err) != FileReader::Okay) {
      if (ok) {
        ok = false;
        *err = "restore cwd = '" + prev_cwd + "': " + dir_err;
      }
    }
  }
  return ok;
}
