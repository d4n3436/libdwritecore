#include "fault_report.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <link.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

namespace fault_report {
namespace {

// A LOAD segment, kept as the numbers needed to turn a runtime address into
// the file offset a disassembler wants.
struct Segment
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t offset;   // p_offset - p_vaddr, added to an image-relative address
};

struct Module
{
    const char* name;
    Segment segment[8];
    unsigned segments;
};

constexpr unsigned kMaxModules = 96;
Module g_module[kMaxModules];
unsigned g_modules = 0;

// Everything the handler needs is collected here, because nothing may be
// allocated or looked up once a fault is in progress.
int g_probe[2] = {-1, -1};
struct sigaction g_previous;
char g_stack[64 * 1024];
volatile sig_atomic_t g_reporting = 0;
bool g_armed = false;

int NoteModule(dl_phdr_info* info, size_t, void*)
{
    if (g_modules >= kMaxModules) {
        return 1;
    }
    Module& m = g_module[g_modules];
    m.name = info->dlpi_name != nullptr && info->dlpi_name[0] != '\0' ? info->dlpi_name
                                                                     : "(executable)";
    m.segments = 0;
    for (int i = 0; i < info->dlpi_phnum && m.segments < 8; ++i) {
        const ElfW(Phdr)& p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD) {
            continue;
        }
        Segment& s = m.segment[m.segments++];
        s.start = info->dlpi_addr + p.p_vaddr;
        s.end = s.start + p.p_memsz;
        s.offset = p.p_offset - p.p_vaddr;
    }
    if (m.segments != 0) {
        ++g_modules;
    }
    return 0;
}

// Whether `n` bytes at `at` can be read, asked without reading them. write()
// has the kernel do the load and answers EFAULT where the load would fault,
// which is the same trick libxul_patch.cpp reads foreign memory with. The pipe
// is opened at install time, since a handler may not open one.
bool Readable(const void* at, const size_t n)
{
    if (g_probe[1] < 0) {
        return false;
    }
    const ssize_t wrote = write(g_probe[1], at, n);
    if (wrote <= 0) {
        return false;
    }
    char drain[64];
    (void)read(g_probe[0], drain, static_cast<size_t>(wrote));
    return static_cast<size_t>(wrote) == n;
}

char* Hex(char* out, const uintptr_t value)
{
    *out++ = '0';
    *out++ = 'x';
    int top = 60;
    while (top > 0 && ((value >> top) & 0xF) == 0) {
        top -= 4;
    }
    for (; top >= 0; top -= 4) {
        *out++ = "0123456789abcdef"[(value >> top) & 0xF];
    }
    return out;
}

char* Text(char* out, const char* s)
{
    while (*s != '\0') {
        *out++ = *s++;
    }
    return out;
}

char* Decimal(char* out, unsigned value)
{
    char digits[12];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (n > 0) {
        *out++ = digits[--n];
    }
    return out;
}

// The file offset of `addr`, matching the kernel's own line: the address
// relative to the image plus the segment's p_offset - p_vaddr.
uintptr_t FileOffset(const uintptr_t addr, unsigned* module_out)
{
    for (unsigned i = 0; i < g_modules; ++i) {
        for (unsigned s = 0; s < g_module[i].segments; ++s) {
            const Segment& seg = g_module[i].segment[s];
            if (addr >= seg.start && addr < seg.end) {
                *module_out = i;
                return addr - seg.start + seg.offset;
            }
        }
    }
    *module_out = kMaxModules;
    return 0;
}

void Say(const char* line, const size_t n)
{
    size_t done = 0;
    while (done < n) {
        const ssize_t wrote = write(2, line + done, n - done);
        if (wrote <= 0) {
            return;
        }
        done += static_cast<size_t>(wrote);
    }
}

char* Frame(char* out, const uintptr_t addr)
{
    unsigned which = 0;
    const uintptr_t offset = FileOffset(addr, &which);
    out = Hex(out, addr);
    if (which < kMaxModules) {
        out = Text(out, " ");
        out = Text(out, g_module[which].name);
        out = Text(out, "+");
        out = Hex(out, offset);
    } else {
        out = Text(out, " (no module)");
    }
    return out;
}

void Handler(const int signal_number, siginfo_t* info, void* context)
{
    // A fault inside the report would recurse forever, so the second one goes
    // straight to whoever handled these before.
    if (g_reporting != 0) {
        (void)sigaction(signal_number, &g_previous, nullptr);
        return;
    }
    g_reporting = 1;

    const auto* uc = static_cast<const ucontext_t*>(context);
    const auto* regs = uc->uc_mcontext.gregs;
    const auto rip = static_cast<uintptr_t>(regs[REG_RIP]);
    const auto rsp = static_cast<uintptr_t>(regs[REG_RSP]);
    auto rbp = static_cast<uintptr_t>(regs[REG_RBP]);

    char line[1024];
    char* p = line;
    p = Text(p, "chromium-patch: fault [");
    p = Decimal(p, static_cast<unsigned>(getpid()));
    p = Text(p, "] signal ");
    p = Decimal(p, static_cast<unsigned>(signal_number));
    p = Text(p, " at ");
    p = Hex(p, reinterpret_cast<uintptr_t>(info->si_addr));
    p = Text(p, "\n  rip ");
    p = Frame(p, rip);
    p = Text(p, "\n  rsp ");
    p = Hex(p, rsp);
    p = Text(p, " rbp ");
    p = Hex(p, rbp);
    p = Text(p, " rdi ");
    p = Hex(p, static_cast<uintptr_t>(regs[REG_RDI]));
    p = Text(p, " rsi ");
    p = Hex(p, static_cast<uintptr_t>(regs[REG_RSI]));
    p = Text(p, " rdx ");
    p = Hex(p, static_cast<uintptr_t>(regs[REG_RDX]));
    p = Text(p, "\n");
    Say(line, static_cast<size_t>(p - line));

    // The frame pointer chain. Chromium keeps frame pointers on this platform,
    // so every frame is two words at rbp, and each one is checked before it is
    // followed because a fault here has nowhere left to go.
    for (unsigned depth = 0; depth < 48; ++depth) {
        if ((rbp & 7) != 0 || !Readable(reinterpret_cast<const void*>(rbp), 16)) {
            break;
        }
        uintptr_t next = 0;
        uintptr_t ret = 0;
        std::memcpy(&next, reinterpret_cast<const void*>(rbp), sizeof(next));
        std::memcpy(&ret, reinterpret_cast<const void*>(rbp + 8), sizeof(ret));
        if (ret == 0) {
            break;
        }
        p = line;
        p = Text(p, "  #");
        p = Decimal(p, depth);
        p = Text(p, " ");
        p = Frame(p, ret);
        p = Text(p, "\n");
        Say(line, static_cast<size_t>(p - line));
        if (next <= rbp || next - rbp > 1u << 22) {
            break;
        }
        rbp = next;
    }

    // Behavior is preserved by letting the fault happen again with whoever was
    // handling it before back in place.
    (void)sigaction(signal_number, &g_previous, nullptr);
    g_reporting = 0;
}

// Take SIGSEGV, remembering whoever held it so the fault can be handed back.
void Arm()
{
    struct sigaction action{};
    action.sa_sigaction = &Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    (void)sigemptyset(&action.sa_mask);
    struct sigaction was{};
    (void)sigaction(SIGSEGV, &action, &was);
    if (was.sa_sigaction != &Handler) {
        g_previous = was;
    }
}

bool Wanted()
{
    const char* v = getenv("DWC_FAULT_REPORT");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0 &&
           std::strcmp(v, "off") != 0;
}

}  // namespace

void InstallAtLoad()
{
    if (!Wanted()) {
        return;
    }
    dl_iterate_phdr(&NoteModule, nullptr);
    if (pipe2(g_probe, O_CLOEXEC | O_NONBLOCK) != 0) {
        g_probe[0] = -1;
        g_probe[1] = -1;
    }
    stack_t alt{};
    alt.ss_sp = g_stack;
    alt.ss_size = sizeof(g_stack);
    (void)sigaltstack(&alt, nullptr);
    g_armed = true;
    Arm();
}

void Ensure()
{
    // Settled once the handler has been found to be ours a few times running,
    // since the host installs its crash handling during startup and never
    // again. Until then this costs one sigaction query per call.
    static std::atomic settled{0};
    if (!g_armed || settled.load(std::memory_order_relaxed) >= 8) {
        return;
    }
    // The host installs its own crash handling after the library is loaded,
    // and a renderer is forked from a process that had already done so, which
    // is why this is asked again from a path that runs once the browser is up.
    struct sigaction now{};
    if (sigaction(SIGSEGV, nullptr, &now) == 0 && now.sa_sigaction == &Handler) {
        settled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    settled.store(0, std::memory_order_relaxed);
    // The altstack belongs to the thread, so a thread that has none gets one.
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) != 0) {
        stack_t alt{};
        alt.ss_sp = g_stack;
        alt.ss_size = sizeof(g_stack);
        (void)sigaltstack(&alt, nullptr);
    }
    Arm();
}

}  // namespace fault_report
