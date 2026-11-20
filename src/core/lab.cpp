#include "core/lab.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

#include "core/log.h"

namespace aco {
namespace {

std::wstring g_lab;  // Z:\lab, empty outside the lab
std::mutex g_mu;
std::map<std::string, LabVerb> g_verbs;
std::set<std::wstring> g_busy;  // request files being served

// Present-thread jobs.
struct Job {
  std::function<void()> fn;
  bool done = false;
};
std::mutex g_jobs_mu;
std::condition_variable g_jobs_cv;
std::deque<std::shared_ptr<Job>> g_jobs;
std::atomic<uint64_t> g_presents{0};

std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

void write_atomic(const std::wstring& path, const std::string& body) {
  std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    f << body;
  }
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Minimal reader for {"id":"..","verb":"..","args":["..",..]}: string values only.
struct Req {
  std::string id, verb;
  std::vector<std::string> args;
};

bool read_jstr(const std::string& s, size_t& i, std::string& out) {
  while (i < s.size() && s[i] != '"') i++;
  if (i >= s.size()) return false;
  i++;
  out.clear();
  while (i < s.size() && s[i] != '"') {
    if (s[i] == '\\' && i + 1 < s.size()) {
      i++;
      char c = s[i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      out += s[i];
    }
    i++;
  }
  i++;
  return true;
}

bool parse_req(const std::string& s, Req& r) {
  size_t i = 0;
  std::string key;
  while (read_jstr(s, i, key)) {
    while (i < s.size() && s[i] != ':') i++;
    i++;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) i++;
    if (i >= s.size()) break;
    if (key == "args" && s[i] == '[') {
      size_t end = s.find(']', i);
      std::string v;
      while (i < end) {
        size_t q = s.find('"', i);
        if (q == std::string::npos || q > end) break;
        i = q;
        read_jstr(s, i, v);
        r.args.push_back(v);
      }
      i = end + 1;
    } else if (s[i] == '"') {
      std::string v;
      read_jstr(s, i, v);
      if (key == "id") r.id = v;
      if (key == "verb") r.verb = v;
    } else {
      size_t e = s.find_first_of(",}", i);
      std::string v = s.substr(i, e - i);
      if (key == "id") r.id = v;
      i = e;
    }
  }
  return !r.verb.empty();
}

void serve(const std::wstring& file, const std::string& text) {
  Req r;
  if (!parse_req(text, r)) {
    ACO_LOG("lab: unreadable request %s", narrow(file).c_str());
    return;
  }
  LabVerb fn;
  {
    std::lock_guard lk(g_mu);
    auto it = g_verbs.find(r.verb);
    if (it != g_verbs.end()) fn = it->second;
  }
  std::string body;
  if (!fn) {
    body = "{\"ok\":false,\"error\":" + json_str("unknown verb " + r.verb) + "}";
  } else {
    bool ok = true;
    std::string res;
    try {
      res = fn(r.args, ok);
    } catch (...) {
      ok = false;
      res = json_str("exception");
    }
    if (ok)
      body = "{\"ok\":true,\"result\":" + (res.empty() ? std::string("null") : res) + "}";
    else
      body = "{\"ok\":false,\"error\":" + (res.empty() ? json_str("failed") : res) + "}";
  }
  ACO_LOG("lab: %s -> %.200s", r.verb.c_str(), body.c_str());
  write_atomic(g_lab + L"\\rsp\\" + widen(r.id) + L".json", body);
}

struct Task {
  std::wstring file;
  std::string text;
};

DWORD WINAPI serve_thread(void* p) {
  std::unique_ptr<Task> t(static_cast<Task*>(p));
  serve(t->file, t->text);
  std::lock_guard lk(g_mu);
  g_busy.erase(t->file);
  return 0;
}

DWORD WINAPI lab_thread(void*) {
  std::wstring dir = g_lab + L"\\req\\";
  for (;;) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::wstring file = dir + fd.cFileName;
        {
          std::lock_guard lk(g_mu);
          if (!g_busy.insert(file).second) continue;
        }
        std::ifstream f(std::filesystem::path(file), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        f.close();
        DeleteFileW(file.c_str());
        auto* t = new Task{file, ss.str()};
        HANDLE th = CreateThread(nullptr, 0, serve_thread, t, 0, nullptr);
        if (th) {
          CloseHandle(th);
        } else {
          serve_thread(t);
        }
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    Sleep(50);
  }
}

}  // namespace

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}

std::string lab_error(bool& ok, const std::string& msg) {
  ok = false;
  return json_str(msg);
}

void lab_init() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"QUIVER_LAB", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  g_lab.assign(buf, n);
  CreateDirectoryW((g_lab + L"\\req").c_str(), nullptr);
  CreateDirectoryW((g_lab + L"\\rsp").c_str(), nullptr);
  ACO_LOG("lab: channel at %s", narrow(g_lab).c_str());
  lab_verb("ping", [](const std::vector<std::string>&, bool&) { return json_str("pong"); });
  lab_state("boot");
  CloseHandle(CreateThread(nullptr, 0, lab_thread, nullptr, 0, nullptr));
}

bool lab_active() { return !g_lab.empty(); }

void lab_state(const std::string& state, const std::vector<std::string>& kv) {
  if (g_lab.empty()) return;
  std::string body = state + "\n";
  for (auto& l : kv) body += l + "\n";
  write_atomic(g_lab + L"\\state", body);
  ACO_LOG("lab: state %s", state.c_str());
}

void lab_verb(const std::string& verb, LabVerb fn) {
  std::lock_guard lk(g_mu);
  g_verbs[verb] = std::move(fn);
}

bool on_present(const std::function<void()>& fn, unsigned timeout_ms) {
  auto job = std::make_shared<Job>();
  job->fn = fn;
  std::unique_lock lk(g_jobs_mu);
  g_jobs.push_back(job);
  if (g_jobs_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return job->done; })) return true;
  // Not run in time: withdraw it, unless the render thread has just taken it.
  for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it)
    if (*it == job) {
      g_jobs.erase(it);
      return false;
    }
  g_jobs_cv.wait(lk, [&] { return job->done; });
  return true;
}

uint64_t present_count() { return g_presents.load(); }

bool wait_presents(uint64_t n, unsigned timeout_ms) {
  uint64_t until = g_presents.load() + n;
  std::unique_lock lk(g_jobs_mu);
  return g_jobs_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return g_presents.load() >= until; });
}

void lab_on_present() {
  g_presents.fetch_add(1);
  if (g_lab.empty()) return;
  std::deque<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard lk(g_jobs_mu);
    jobs.swap(g_jobs);
  }
  for (auto& j : jobs) j->fn();
  {
    std::lock_guard lk(g_jobs_mu);
    for (auto& j : jobs) j->done = true;
  }
  g_jobs_cv.notify_all();
}

}  // namespace aco
