// receiver_win32_fixed.cpp
// -----------------------------------------------------------------------------
// Win32-compatible TCP receiver (MinGW-friendly)
// -----------------------------------------------------------------------------
// Behavior & protocol (DO NOT CHANGE):
//  - Listens for a TCP connection on a specified port (default 5001).
//  - Expects a 4-byte big-endian unsigned length prefix followed by payload bytes.
//  - Receives payload reliably (handles partial recv), writes payload atomically to disk.
//  - Optionally sends a 1-byte ACK (0x01) back to the client after successful save.
//  - After a file is received and saved, wait for hotkey 7+8+9 on the laptop to
//    type the saved file's UTF-8 text into the currently focused window using SendInput.
// -----------------------------------------------------------------------------
// Usage / build:
//   g++ -std=c++17 receiver_win32_fixed.cpp -o receiver.exe -lws2_32
//   receiver.exe --port 5001 --out "received_data.txt" [--no-ack] [--postcmd "cmd"]
// -----------------------------------------------------------------------------
// Notes:
//  - This file is intentionally written for compatibility with older MinGW toolchains.
//  - It uses Win32 threads (CreateThread) and CRITICAL_SECTION instead of std::thread/mutex.
//  - Keep semantics and command-line options identical to the previous working file.
// -----------------------------------------------------------------------------
// NOTE ON CODING STYLE:
//
// This project deliberately avoids `using namespace std;` and instead uses
// explicit `std::` qualifiers everywhere.
//
// Reason:
//   - In real-world C++ network/system code, pulling the entire std namespace
//     into the global scope is discouraged because it increases the chance of
//     name collisions (especially with Win32 APIs, socket functions, and macros).
//   - Several identifiers used here (e.g., time, out, string, vector) also exist
//     in Windows headers or C libraries, so explicit `std::` makes the code safer
//     and removes ambiguity.
//   - This style is common in production-grade C++ codebases where clarity and
//     namespace hygiene matter more than brevity.
//
// This is why the implementation consistently uses `std::string`,
// `std::vector`, `std::ostringstream`, etc.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // WinSock2 main
#include <ws2tcpip.h>   // inet_ntop
#include <windows.h>    // Win32 API (Sleep, CreateThread, SendInput, etc.)

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <ctime>        // time, localtime, tm, strftime
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>

#pragma comment(lib, "ws2_32.lib") // Link with WinSock2

// ------------------------- Configuration defaults ----------------------------
static const uint16_t PORT_DEFAULT = 5001;               // default listening port
static const std::string OUT_DEFAULT = "received_data.txt"; // default output filename
static const int SOCKET_TIMEOUT_SECONDS = 10;            // socket receive timeout (seconds)
static const bool SEND_ACK_DEFAULT = true;               // whether to send 1-byte ACK after save

// ------------------------------ Global state --------------------------------
// Atomic flags and shared state used between server thread and main thread
std::atomic<bool> g_should_terminate(false); // when true, program should stop
std::atomic<bool> g_file_received(false);    // set to true when a file was saved
std::string g_last_received_path;            // path to the last saved file
CRITICAL_SECTION g_path_cs;                  // protects g_last_received_path
bool g_send_ack = SEND_ACK_DEFAULT;          // runtime ACK option
std::string g_post_cmd = "";                 // optional post command to run after save

// ------------------------------ Logging helpers ------------------------------

// Portable localtime wrapper: uses localtime_s on MSVC, or std::localtime copy on MinGW.
void get_localtime_safe(struct tm &out, time_t t) {
#ifdef _MSC_VER
    localtime_s(&out, &t);
#else
    std::tm *tmp = std::localtime(&t);
    if (tmp) out = *tmp;
    else std::memset(&out, 0, sizeof(out));
#endif
}

// Formats a timestamped log line and writes to stdout/stderr depending on level.
void log_msg_str(const char *level, const std::string &msg) {
    time_t now = time(NULL);
    struct tm lt;
    get_localtime_safe(lt, now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%F %T", &lt);

    std::ostringstream oss;
    oss << timebuf << " [" << level << "] " << msg << "\n";
    std::string s = oss.str();
    if (strcmp(level, "ERROR") == 0) {
        fwrite(s.c_str(), 1, s.size(), stderr);
    } else {
        fwrite(s.c_str(), 1, s.size(), stdout);
    }
}

// Convenience macros for logging at different levels
#define log_info(s) log_msg_str("INFO", (s))
#define log_warn(s) log_msg_str("WARN", (s))
#define log_err(s)  log_msg_str("ERROR", (s))

// ------------------------------ Networking helpers ---------------------------

// recv_all: receive exactly nbytes into 'out' (handles partial reads).
// Returns true on success, false on error/timeouts.
bool recv_all(SOCKET s, std::vector<uint8_t> &out, size_t nbytes, int timeout_seconds) {
    out.clear();
    out.reserve(nbytes);
    size_t total = 0;
    DWORD start = GetTickCount(); // millisecond tick to check timeout

    while (total < nbytes) {
        // request at most 4096 bytes per recv iteration
        int torecv = static_cast<int>(std::min<size_t>(4096, nbytes - total));
        std::vector<uint8_t> buf(torecv);

        int r = ::recv(s, reinterpret_cast<char*>(buf.data()), torecv, 0);
        if (r == 0) {
            // peer closed connection
            log_warn("recv_all: connection closed by peer");
            return false;
        } else if (r < 0) {
            int err = WSAGetLastError();
            // transient conditions: would block or timed out; retry until deadline
            if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
                if ((GetTickCount() - start) > static_cast<DWORD>(timeout_seconds * 1000)) {
                    log_err("recv_all: timeout waiting for data");
                    return false;
                }
                Sleep(20);
                continue;
            }
            std::ostringstream os; os << "recv_all: recv error (" << err << ")";
            log_err(os.str());
            return false;
        } else {
            // append received bytes to out
            out.insert(out.end(), buf.data(), buf.data() + r);
            total += r;
            // reset deadline on activity
            start = GetTickCount();
        }
    }

    return true;
}

// recv_uint32_be: read a 4-byte big-endian unsigned integer from socket
bool recv_uint32_be(SOCKET s, uint32_t &out_len, int timeout_seconds) {
    uint8_t buf[4];
    size_t got = 0;
    DWORD start = GetTickCount();

    while (got < 4) {
        int r = ::recv(s, reinterpret_cast<char*>(buf + got), static_cast<int>(4 - got), 0);
        if (r <= 0) {
            int err = WSAGetLastError();
            // allow transient timeouts
            if (r == 0 || err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
                if ((GetTickCount() - start) > static_cast<DWORD>(timeout_seconds * 1000)) {
                    log_err("recv_uint32_be: timeout or closed");
                    return false;
                }
                Sleep(10);
                continue;
            }
            std::ostringstream os; os << "recv_uint32_be: recv failed err=" << err;
            log_err(os.str());
            return false;
        }
        got += r;
    }

    // assemble big-endian uint32
    out_len = (static_cast<uint32_t>(buf[0]) << 24) |
              (static_cast<uint32_t>(buf[1]) << 16) |
              (static_cast<uint32_t>(buf[2]) << 8)  |
              (static_cast<uint32_t>(buf[3]));
    return true;
}

// write_file_atomic: write file to a temporary file and rename it to the target name.
// This reduces the chance of producing a corrupted partial file on disk.
bool write_file_atomic(const std::string &path, const std::vector<uint8_t> &data) {
    std::string tmp = path + ".tmp";
    std::ofstream ofs(tmp.c_str(), std::ios::binary);
    if (!ofs) {
        std::ostringstream os; os << "Failed to open temp file " << tmp;
        log_err(os.str());
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
    ofs.close();

    // atomic replace on Windows
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD err = GetLastError();
        std::ostringstream os; os << "MoveFileExA failed err=" << err;
        log_err(os.str());
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// ------------------------------ Core client handler --------------------------

// handle_single_client: receives one full length-prefixed payload and writes it to out_path.
// If g_send_ack is true, sends a single byte 0x01 ACK to client after successfully saving.
bool handle_single_client(SOCKET client_sock, const std::string &out_path) {
    // set receive timeout for safety
    DWORD to_ms = SOCKET_TIMEOUT_SECONDS * 1000;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to_ms, sizeof(to_ms));

    log_info("Client connected: reading 4-byte length");
    uint32_t payload_len = 0;
    if (!recv_uint32_be(client_sock, payload_len, SOCKET_TIMEOUT_SECONDS)) {
        log_err("Failed to read payload length");
        return false;
    }

    {
        std::ostringstream os; os << "Payload length = " << payload_len << " bytes";
        log_info(os.str());
    }

    // simple sanity check for payload size (prevent runaway allocation)
    const uint32_t MAX_REASONABLE_PAYLOAD = 50u * 1024u * 1024u; // 50 MB
    if (payload_len == 0 || payload_len > MAX_REASONABLE_PAYLOAD) {
        log_err("Invalid or too large payload length");
        return false;
    }

    // receive the payload in full
    std::vector<uint8_t> payload;
    if (!recv_all(client_sock, payload, payload_len, SOCKET_TIMEOUT_SECONDS)) {
        log_err("Failed to receive full payload");
        return false;
    }

    // Save atomically to disk
    if (!write_file_atomic(out_path, payload)) {
        log_err("Failed to save received payload to disk");
        return false;
    }

    // Optionally send ACK (1 byte) back to client
    if (g_send_ack) {
        char ack = 0x01;
        int sent = ::send(client_sock, &ack, 1, 0);
        if (sent == 1) log_info("ACK sent to client");
        else log_warn("Failed to send ACK (non-critical)");
    }

    // Update shared last-received path safely under CRITICAL_SECTION
    EnterCriticalSection(&g_path_cs);
    g_last_received_path = out_path;
    LeaveCriticalSection(&g_path_cs);

    // Notify main thread (via atomics) that a file has arrived
    g_file_received.store(true);

    std::ostringstream ok; ok << "Saved file: " << out_path;
    log_info(ok.str());
    return true;
}

// ------------------------------ Server thread --------------------------------

// Parameters passed to the CreateThread server thread function
struct ServerParams {
    uint16_t port;
    std::string out_path;
    int backlog;
};

// server_thread_func: runs in a dedicated thread. It listens, accepts a single
// connection, handles it (blocking), then continues the loop. The loop uses
// select() with timeout so it can periodically check for program termination.
DWORD WINAPI server_thread_func(LPVOID param) {
    ServerParams *p = reinterpret_cast<ServerParams*>(param);
    uint16_t port = p->port;
    std::string out_path = p->out_path;
    int backlog = p->backlog;
    delete p; // ownership transferred to thread; free params

    std::ostringstream start; start << "Server thread starting on port " << port;
    log_info(start.str());

    while (!g_should_terminate.load()) {
        // create listening socket
        SOCKET listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET) {
            std::ostringstream os; os << "socket() failed err=" << WSAGetLastError();
            log_err(os.str());
            Sleep(500);
            continue;
        }

        BOOL reuse = TRUE;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

        // bind to INADDR_ANY:port
        sockaddr_in listen_addr;
        ZeroMemory(&listen_addr, sizeof(listen_addr));
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_port = htons(port);
        listen_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(listen_sock, reinterpret_cast<sockaddr*>(&listen_addr), sizeof(listen_addr)) == SOCKET_ERROR) {
            std::ostringstream os; os << "bind() failed err=" << WSAGetLastError();
            log_err(os.str());
            closesocket(listen_sock);
            Sleep(1000);
            continue;
        }

        if (listen(listen_sock, backlog) == SOCKET_ERROR) {
            log_err("listen() failed");
            closesocket(listen_sock);
            Sleep(1000);
            continue;
        }

        // select() with 1s timeout so we can check g_should_terminate periodically
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(listen_sock, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(0, &rf, NULL, NULL, &tv);
        if (sel == 0) {
            // no incoming connection in this interval; close the listen socket and reloop
            closesocket(listen_sock);
            continue;
        } else if (sel == SOCKET_ERROR) {
            std::ostringstream os; os << "select() failed err=" << WSAGetLastError();
            log_err(os.str());
            closesocket(listen_sock);
            Sleep(200);
            continue;
        }

        // Accept incoming connection (blocking here)
        sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        SOCKET client_sock = ::accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
        if (client_sock == INVALID_SOCKET) {
            log_err("accept() failed");
            closesocket(listen_sock);
            continue;
        }

        // Log client IP address (inet_ntoa used for compatibility)
        char *client_ip = inet_ntoa(client_addr.sin_addr);
        {
            std::ostringstream os;
            os << "Accepted connection from " << (client_ip ? client_ip : "unknown") << ":" << ntohs(client_addr.sin_port);
            log_info(os.str());
        }

        // Handle the client connection (blocking) - receives payload & saves it
        handle_single_client(client_sock, out_path);

        // Close client and listen sockets; then short sleep to avoid busy-loop
        closesocket(client_sock);
        closesocket(listen_sock);
        Sleep(100);
    }

    log_info("Server thread shutting down");
    return 0;
}

// start_server_thread: helper to create the server thread via CreateThread
HANDLE start_server_thread(uint16_t port, const std::string &out_path, int backlog) {
    ServerParams *p = new ServerParams();
    p->port = port;
    p->out_path = out_path;
    p->backlog = backlog;
    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, server_thread_func, p, 0, &tid);
    if (!th) {
        delete p;
        std::ostringstream os; os << "CreateThread failed err=" << GetLastError();
        log_err(os.str());
    }
    return th;
}

// ------------------------------ Hotkey detection -----------------------------

// hotkey_789_pressed: returns true when keys '7', '8', and '9' are all pressed
// Uses GetAsyncKeyState to poll keyboard state.
bool hotkey_789_pressed() {
    return (GetAsyncKeyState('7') & 0x8000) &&
           (GetAsyncKeyState('8') & 0x8000) &&
           (GetAsyncKeyState('9') & 0x8000);
}

// ------------------------------ Typing helper -------------------------------

// type_file_into_active_window: reads UTF-8 file at 'path', converts to UTF-16 and
// sends keyboard events (unicode) to the active window using SendInput.
bool type_file_into_active_window(const std::string &path) {
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs) {
        log_err(std::string("Cannot open file to type: ") + path);
        return false;
    }

    // load full contents (assumes text reasonably sized)
    std::string contents((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    if (contents.empty()) {
        log_info("File is empty, nothing to type");
        return true;
    }

    // convert UTF-8 -> UTF-16 (wchar_t)
    int needed = MultiByteToWideChar(CP_UTF8, 0, contents.data(), static_cast<int>(contents.size()), NULL, 0);
    if (needed == 0) {
        log_err("MultiByteToWideChar size error");
        return false;
    }
    std::vector<wchar_t> wbuf(needed);
    if (MultiByteToWideChar(CP_UTF8, 0, contents.data(), static_cast<int>(contents.size()), wbuf.data(), needed) == 0) {
        log_err("MultiByteToWideChar conversion failed");
        return false;
    }

    // Build INPUT events: for each wchar, send a KEY event then KEYUP event
    std::vector<INPUT> inputs;
    inputs.reserve(needed * 2);
    for (int i = 0; i < needed; ++i) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = wbuf[i];
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = {};
        up.type = INPUT_KEYBOARD;
        up.ki.wScan = wbuf[i];
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }

    // Send inputs in batches to avoid large one-shot calls
    const size_t batch = 200;
    size_t idx = 0;
    while (idx < inputs.size()) {
        size_t tosend = std::min(batch, inputs.size() - idx);
        UINT sent = SendInput(static_cast<UINT>(tosend), &inputs[idx], sizeof(INPUT));
        if (sent != tosend) {
            log_warn("SendInput sent fewer events than expected");
        }
        idx += tosend;
        Sleep(10); // small pause between batches
    }

    log_info("Typing completed");
    return true;
}

// ------------------------------ CLI parsing ---------------------------------
void print_usage(const char *prog) {
    std::cout << "Usage: " << prog << " [--port PORT] [--out FILE] [--no-ack] [--postcmd CMD]\n";
}
struct Options { uint16_t port; std::string out_file; bool no_ack; std::string postcmd; };

// parse_args: minimal command-line parsing; preserves previous options exactly
Options parse_args(int argc, char **argv) {
    Options opt;
    opt.port = PORT_DEFAULT;
    opt.out_file = OUT_DEFAULT;
    opt.no_ack = false;
    opt.postcmd = "";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) opt.port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc) opt.out_file = argv[++i];
        else if (a == "--no-ack") opt.no_ack = true;
        else if (a == "--postcmd" && i + 1 < argc) opt.postcmd = argv[++i];
        else if (a == "--help") { print_usage(argv[0]); exit(0); }
    }
    return opt;
}

// ------------------------------ Post-command runner -------------------------

// Run a user-specified command asynchronously via CreateThread wrapper.
// The thread just runs system(cmd) and exits.
struct PostCmdParam { std::string cmd; };

DWORD WINAPI postcmd_thread_func(LPVOID param) {
    PostCmdParam *p = reinterpret_cast<PostCmdParam*>(param);
    if (p && !p->cmd.empty()) {
        std::ostringstream os; os << "Running post command: " << p->cmd; log_info(os.str());
        int rc = system(p->cmd.c_str());
        std::ostringstream rcmsg; rcmsg << "Post command returned rc=" << rc; log_info(rcmsg.str());
    }
    delete p;
    return 0;
}

void run_post_command_async(const std::string &cmd) {
    if (cmd.empty()) return;
    PostCmdParam *p = new PostCmdParam();
    p->cmd = cmd;
    DWORD tid = 0;
    HANDLE h = CreateThread(NULL, 0, postcmd_thread_func, p, 0, &tid);
    if (!h) { delete p; log_warn("Failed to create postcmd thread"); }
    else CloseHandle(h);
}

// ------------------------------ main ---------------------------------------

int main(int argc, char **argv) {
    // Parse command-line options and set runtime flags
    Options opt = parse_args(argc, argv);
    g_send_ack = !opt.no_ack;
    g_post_cmd = opt.postcmd;
    g_last_received_path = opt.out_file;

    // Initialize CRITICAL_SECTION used for protecting the shared filename string
    InitializeCriticalSection(&g_path_cs);

    std::ostringstream startmsg;
    startmsg << "Receiver starting on port " << opt.port << " saving to '" << opt.out_file << "'";
    log_info(startmsg.str());

    // Initialize WinSock (must be called before socket operations)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        log_err("WSAStartup failed");
        return 1;
    }

    // Start the server thread (CreateThread wrapper)
    HANDLE serverHandle = start_server_thread(opt.port, opt.out_file, 1);

    // Main loop: wait for a file to be received, then wait for hotkey to type it
    while (!g_should_terminate.load()) {
        if (g_file_received.load()) {
            // copy path safely under CRITICAL_SECTION
            EnterCriticalSection(&g_path_cs);
            std::string path = g_last_received_path;
            LeaveCriticalSection(&g_path_cs);
            g_file_received.store(false); // reset flag

            std::ostringstream os; os << "File received: " << path << " (press 7+8+9 to type)";
            log_info(os.str());

            // Optionally run the user-provided post-command (non-blocking)
            if (!g_post_cmd.empty()) run_post_command_async(g_post_cmd);

            // Wait for the hotkey 7+8+9 and then type the file into active window
            bool done = false;
            while (!done && !g_should_terminate.load()) {
                if (hotkey_789_pressed()) {
                    log_info("Hotkey pressed - typing file");
                    if (!type_file_into_active_window(path)) log_err("Typing failed");
                    // wait until keys are released to avoid repeated typing
                    while (hotkey_789_pressed()) Sleep(100);
                    done = true;
                }
                Sleep(50);
            }
        } else {
            // Sleep to reduce CPU usage while idle
            Sleep(200);
        }
    }

    // Shutdown sequence: signal termination and wait for server thread to wrap up
    g_should_terminate.store(true);
    if (serverHandle) {
        WaitForSingleObject(serverHandle, 2000);
        CloseHandle(serverHandle);
    }

    WSACleanup(); // cleanup WinSock
    DeleteCriticalSection(&g_path_cs);
    log_info("Receiver exiting");
    return 0;
}
