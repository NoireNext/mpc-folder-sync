// MPC Folder Sync - ein MPC-Instrument-Plugin von Noirescripts
//
// Was es tut:
//   Das Plugin erzeugt keinen Klang. Per Knopfdruck startet es rsync (über SSH) und kopiert
//   einen Ordner der MPC auf einen Server / ein NAS. Ziele, Adresse, Ordner und SSH-Schlüssel
//   stehen in Content/foldersync.conf, weil Plugins auf der MPC keine Texteingabe haben.
//   Auf der MPC wählt man das Ziel (Target 1..8), sieht Name, Quelle und Ziel als Text und
//   drückt den Button "Start Sync".
//
//   Die MPC bringt weder rsync noch ssh mit. Darum liegen statisch gebaute Programme im Plugin
//   (Content/bin/arm32 und arm64): rsync und "dropbearmulti" (SSH-Client dbclient + dropbearconvert).
//   Sie werden für die Dauer eines Syncs nach /tmp kopiert und von dort gestartet, weil Laufwerke
//   (FAT/exFAT) oft ohne Ausführrecht eingehängt sind.
//
// Ablauf:
//   Start Sync: Aus -> An   (parameterChanged, nur ein Flag setzen)
//     -> Worker-Thread (eigener Thread, NIE der Audio-Thread) liest die Konfiguration,
//        kopiert rsync/dbclient nach /tmp, übersetzt bei Bedarf den SSH-Schlüssel, startet rsync
//        mit posix_spawn und wartet darauf, schreibt die Ausgabe nach Content/foldersync.log
//     -> Status zeigt Running, danach Done oder Failed (bzw. den Grund), Start Sync springt zurück auf Aus.
//
// Regeln (siehe docs/host-contract.md):
//   * process() macht nichts: keine Dateien, kein Logging, keine Allokation.
//   * Start Sync löst nur aus, wenn das Plugin bereit ist (nach prepare()), damit ein geladenes
//     Projekt nie von selbst einen Sync startet.
//   * Die Textanzeigen (Name, Quelle, Ziel) sind Parameter ohne eigene Bedeutung: ihr Anzeigetext
//     kommt aus formatValue(); ihr Wert wird nur verändert, damit die MPC den Text neu liest.

#include "mpc_plugin.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32)
  #include <fcntl.h>
  #include <pthread.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #define FOLDERSYNC_CAN_SPAWN 1
#endif

namespace {

// Reihenfolge = Reihenfolge der addParam()-Aufrufe. Nie ändern, nur hinten anhängen.
enum ParamIndex
{
    kTarget = 0,   // 1..8: welches Ziel aus foldersync.conf
    kStart,        // Button: Aus -> An startet den Sync, leuchtet solange er läuft
    kStatus,       // Anzeige: Ready / Running / Done / ...
    kDryRun,       // An = rsync -n (nur anzeigen, nichts kopieren)
    kSource,       // reine Textanzeige: Quellordner auf der MPC
    kDest,         // reine Textanzeige: Ziel (user@host:ordner)
    kTargetName,   // reine Textanzeige: Name des gewählten Ziels
    kJob1,         // kJob1 .. kJob1+7: reine Textanzeige, eine Zeile der Jobliste pro Ziel
    kJobLast = kJob1 + 7,
    kProgress,     // reine Textanzeige: Fortschritt in Prozent während des Syncs
    kJobA1,        // kJobA1 .. kJobA1+7: wie Job 1-8, aber nur die aktive Zeile hat Text (wird farbig angezeigt)
    kJobALast = kJobA1 + 7,
    kNumParams
};

enum StatusValue
{
    kStReady = 0, kStRunning, kStDone, kStFailed,
    kStNoRsync, kStNoSsh, kStNoConfig, kStBadTarget, kStNoExec
};

constexpr int         kMaxTargets = 8;
constexpr const char* kConfigName = "foldersync.conf";
constexpr const char* kLogName    = "foldersync.log";

// Mitgelieferte Programme: je nach Bauart des Plugins der passende Ordner in Content/.
// "native" gibt es nur für Tests auf dem Entwicklungsrechner.
#if defined(__aarch64__)
constexpr const char* kBinDir = "bin/arm64";
#elif defined(__arm__)
constexpr const char* kBinDir = "bin/arm32";
#else
constexpr const char* kBinDir = "bin/native";
#endif

// ---------------------------------------------------------------------------
// Konfiguration (Content/foldersync.conf)
// ---------------------------------------------------------------------------
struct Target
{
    std::string name, host, user, port = "22", source, dest, key, password, sshCommand, rsyncPath;
    std::vector<std::string> excludes;
    bool        del = false;
};

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Einfaches Format: [Name] startet ein Ziel, darunter "schluessel = wert". Zeilen mit # sind Kommentare.
void parseConfig(const std::string& text, std::vector<Target>& out)
{
    out.clear();
    size_t pos = 0;
    while (pos <= text.size())
    {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[' && line.back() == ']')
        {
            if ((int)out.size() >= kMaxTargets) break;
            Target t;
            t.name = trim(line.substr(1, line.size() - 2));
            out.push_back(t);
            continue;
        }
        if (out.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string k = lower(trim(line.substr(0, eq)));
        const std::string v = trim(line.substr(eq + 1));
        Target& t = out.back();
        if      (k == "host")        t.host = v;
        else if (k == "user")        t.user = v;
        else if (k == "port")        t.port = v;
        else if (k == "source")      t.source = v;
        else if (k == "dest")        t.dest = v;
        else if (k == "key")         t.key = v;
        else if (k == "password")    t.password = v;
        else if (k == "ssh_command") t.sshCommand = v;
        else if (k == "rsync_path")  t.rsyncPath = v;
        else if (k == "exclude")     { if (!v.empty() && t.excludes.size() < 64) t.excludes.push_back(v); }
        else if (k == "delete")      { const std::string l = lower(v); t.del = (l == "yes" || l == "true" || l == "1" || l == "on"); }
    }
}

bool isSafeName(const std::string& s)
{
    if (s.empty() || s[0] == '-') return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return false;
    return true;
}

bool isPort(const std::string& s)
{
    if (s.empty() || s.size() > 5) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// Pfad des rsync auf dem Ziel (--rsync-path): nur harmlose Zeichen, damit nichts "durchrutscht".
bool isSafeRemotePath(const std::string& s)
{
    for (char c : s)
        if (!(std::isalnum((unsigned char)c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    return true;
}

// Die MPC zeigt Parametertexte nur bis ca. 31 Zeichen. Bei langen Pfaden ist das Ende wichtiger als der Anfang.
std::string fitText(const std::string& s)
{
    constexpr size_t kMax = 31;
    if (s.size() <= kMax) return s;
    return "..." + s.substr(s.size() - (kMax - 3));
}

// Namen: der Anfang ist wichtiger als das Ende.
std::string fitHead(const std::string& s, size_t maxChars)
{
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars - 3) + "...";
}

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
}

#ifdef FOLDERSYNC_CAN_SPAWN

// ---------------------------------------------------------------------------
// Log-Datei: Content/foldersync.log (wird bei > 200 KB neu begonnen)
// ---------------------------------------------------------------------------
class SyncLog
{
public:
    explicit SyncLog(const std::string& path)
    {
        struct stat st;
        const char* mode = (stat(path.c_str(), &st) == 0 && st.st_size > 200000) ? "w" : "a";
        f_ = std::fopen(path.c_str(), mode);
    }
    ~SyncLog() { if (f_ != nullptr) std::fclose(f_); }
    SyncLog(const SyncLog&) = delete;
    SyncLog& operator=(const SyncLog&) = delete;

    void line(const char* fmt, ...)
    {
        if (f_ == nullptr) return;
        va_list args;
        va_start(args, fmt);
        std::vfprintf(f_, fmt, args);
        va_end(args);
        std::fputc('\n', f_);
        std::fflush(f_);
    }
    int fd() const { return f_ != nullptr ? fileno(f_) : -1; }

private:
    std::FILE* f_ = nullptr;
};

std::string searchPath()
{
    std::string paths = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:/opt/bin";
    if (const char* p = std::getenv("PATH")) paths = std::string(p) + ":" + paths;
    return paths;
}

// Gibt den vollen Pfad eines Programms zurück, oder "" wenn es nicht auffindbar/ausführbar ist.
std::string findExecutable(const std::string& name)
{
    if (name.empty()) return std::string();
    if (name[0] == '/') return access(name.c_str(), X_OK) == 0 ? name : std::string();
    const std::string paths = searchPath();
    size_t start = 0;
    while (start <= paths.size())
    {
        size_t end = paths.find(':', start);
        if (end == std::string::npos) end = paths.size();
        if (end > start)
        {
            const std::string candidate = paths.substr(start, end - start) + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        start = end + 1;
    }
    return std::string();
}

// Schreibt eine Datei mit festen Rechten (umask-unabhängig).
bool writeFileMode(const std::string& path, const std::vector<uint8_t>& bytes, mode_t mode)
{
    unlink(path.c_str());
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return false;
    size_t done = 0;
    bool ok = true;
    while (done < bytes.size())
    {
        const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += (size_t)n;
    }
    close(fd);
    if (ok) chmod(path.c_str(), mode);
    else    unlink(path.c_str());
    return ok;
}

// Arbeitsordner in /tmp für die Dauer eines Syncs: hier liegen die ausführbaren Kopien von rsync
// und dropbearmulti sowie der (ggf. übersetzte) SSH-Schlüssel mit Rechten 0600/0700. Wird am Ende
// immer aufgeräumt. /tmp statt Laufwerk, weil FAT/exFAT oft "noexec" eingehängt sind und alle Dateien
// als "offen" melden (ssh lehnt solche Schlüssel ab).
class WorkDir
{
public:
    explicit WorkDir(const void* owner)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "/tmp/foldersync-%d-%lx", (int)getpid(), (unsigned long)(uintptr_t)owner);
        path_ = buf;
        cleanup();                                   // Reste eines abgebrochenen Laufs
        ok_ = (mkdir(path_.c_str(), 0700) == 0);
    }
    ~WorkDir() { cleanup(); }
    WorkDir(const WorkDir&) = delete;
    WorkDir& operator=(const WorkDir&) = delete;

    bool ok() const { return ok_; }
    std::string file(const std::string& name) const { return path_ + "/" + name; }

    // Kopiert ein mitgeliefertes Programm aus Content/<kBinDir>/ hierher und macht es ausführbar.
    // Gibt "" zurück, wenn es nicht mitgeliefert ist oder das Kopieren scheitert.
    std::string stage(const char* name, SyncLog& log) const
    {
        if (!ok_) return std::string();
        std::vector<uint8_t> bytes;
        const std::string src = mpc::contentPath(std::string(kBinDir) + "/" + name);
        if (!mpc::readFile(src, bytes) || bytes.empty()) return std::string();
        const std::string dst = file(name);
        if (!writeFileMode(dst, bytes, 0700))
        {
            log.line("%s kann nicht nach %s kopiert werden (%s)", name, dst.c_str(), std::strerror(errno));
            return std::string();
        }
        return dst;
    }

private:
    void cleanup()
    {
        static const char* const names[] = { "rsync", "dropbearmulti", "key", "out" };
        for (const char* n : names) unlink((path_ + "/" + n).c_str());
        rmdir(path_.c_str());
    }

    std::string path_;
    bool        ok_ = false;
};

void logMounts(SyncLog& log)
{
    std::FILE* f = std::fopen("/proc/mounts", "r");
    if (f == nullptr) return;
    log.line("Eingehaengte Laufwerke (Geraet -> Ordner):");
    char l[512];
    while (std::fgets(l, sizeof(l), f) != nullptr)
    {
        if (std::strncmp(l, "/dev/", 5) != 0) continue;
        char dev[256], mp[256];
        if (std::sscanf(l, "%255s %255s", dev, mp) == 2) log.line("  %s -> %s", dev, mp);
    }
    std::fclose(f);
}

#endif // FOLDERSYNC_CAN_SPAWN

} // namespace

class MPCFolderSync : public mpc::Plugin
{
public:
    MPCFolderSync() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::intParam("target", "Target", 1, kMaxTargets, 1));

        mpc::Param start = mpc::boolParam("start", "Start Sync", false);
        start.automatable = false;
        addParam(start);

        mpc::Param status = mpc::choiceParam("status", "Status",
            {"Ready", "Running", "Done", "Failed", "No rsync", "No ssh", "No config", "Bad target", "Cannot run"}, 0);
        status.automatable = false;
        addParam(status);

        addParam(mpc::boolParam("dryrun", "Dry Run", false));

        // Reine Textanzeigen: der Wert bedeutet nichts, der Anzeigetext kommt aus formatValue().
        const struct { const char* id; const char* name; } texts[] = {
            { "source", "Source" }, { "dest", "Destination" }, { "targetname", "Name" } };
        for (const auto& t : texts)
        {
            mpc::Param p = mpc::floatParam(t.id, t.name, 0.0f, 1.0f, 0.0f);
            p.automatable = false;
            addParam(p);
        }
        for (int i = 0; i < kMaxTargets; ++i)
        {
            const std::string id = "job" + std::to_string(i + 1), nm = "Job " + std::to_string(i + 1);
            mpc::Param p = mpc::floatParam(id.c_str(), nm.c_str(), 0.0f, 1.0f, 0.0f);
            p.automatable = false;
            addParam(p);
        }

        {
            mpc::Param p = mpc::floatParam("progress", "Progress", 0.0f, 1.0f, 0.0f);
            p.automatable = false;
            addParam(p);
        }

        for (int i = 0; i < kMaxTargets; ++i)
        {
            const std::string id = "jobactive" + std::to_string(i + 1), nm = "Active Job " + std::to_string(i + 1);
            mpc::Param p = mpc::floatParam(id.c_str(), nm.c_str(), 0.0f, 1.0f, 0.0f);
            p.automatable = false;
            addParam(p);
        }

        refreshTexts(false);                // damit der erste Anzeigetext schon stimmt
        lastTarget_ = getInt(kTarget);

#ifdef FOLDERSYNC_CAN_SPAWN
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&thread_, &attr, &MPCFolderSync::threadEntry, this) == 0) threadStarted_ = true;
        pthread_attr_destroy(&attr);
#endif
    }

    ~MPCFolderSync() override
    {
#ifdef FOLDERSYNC_CAN_SPAWN
        stop_.store(true);
        if (threadStarted_) pthread_join(thread_, nullptr);
#endif
    }

    // Nach dem Laden eines Projekts: nie von selbst starten.
    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)sampleRate; (void)maxBlockSize;
        if (!running_.load())
        {
            set(kStart, 0.0f);
            set(kStatus, (float)kStReady);
        }
        needRefresh_.store(true);
        armed_.store(true);
    }

    // Instrument ohne Klang: die Ausgänge sind schon genullt.
    void process(mpc::ProcessContext& ctx) override { (void)ctx; }

    // Läuft evtl. auf dem Audio-Thread: nur ein Flag setzen, sonst nichts.
    void parameterChanged(int index, float plainValue) override
    {
        if (index == kStart && plainValue >= 0.5f && armed_.load() && !running_.load())
            startPending_.store(true);
    }

    // Anzeigetexte für Name, Quelle und Ziel des gewählten Targets.
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        if (index >= kJob1 && index <= kJobLast) { out = txtJobs_[index - kJob1]; return true; }
        if (index >= kJobA1 && index <= kJobALast) { out = txtJobsActive_[index - kJobA1]; return true; }
        if (index == kTarget)
        {
            // "2  Backup Samples": Nummer und Name des Ziels, auch beim Drehen schon für den neuen Wert.
            const int n = (int)std::lround(plainValue);
            out = std::to_string(n);
            if (n >= 1 && n <= kMaxTargets && !txtNames_[n - 1].empty()) out += "  " + fitHead(txtNames_[n - 1], 13);
            return true;
        }
        if (index == kStatus)
        {
            static const char* const names[] = { "Ready", "Running", "Done", "Failed", "No rsync", "No ssh", "No config", "Bad target", "Cannot run" };
            int st = (int)std::lround(plainValue);
            if (st < 0) st = 0;
            if (st > 8) st = 8;
            out = names[st];
            if (st == kStRunning && txtProgress_ != "-" && !txtProgress_.empty()) out += " " + txtProgress_;
            return true;
        }
        if (index == kProgress) { out = txtProgress_; return true; }
        if (index != kSource && index != kDest && index != kTargetName) return false;
        out = (index == kSource) ? txtSource_ : (index == kDest) ? txtDest_ : txtName_;
        return true;
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "MPC Folder Sync";
        info.vendor       = "Noirescripts";   // muss zu plugin.json "manufacturer" passen
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // muss zu plugin.json "uid" passen
        info.version      = 1400;             // 1.4.0.0
        info.isInstrument = true;             // Instrumente erscheinen als Plugin-Programm, Effekte nur als Insert
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "N2fs";

    void setStatus(int s)
    {
        set(kStatus, (float)s);
        notifyHostParameterChanged(kStatus);
    }

    // Liest die Konfiguration und setzt die Anzeigetexte für das gewählte Target.
    // Mit notify = true wird die MPC zusätzlich angestoßen, die Texte neu zu lesen.
    void refreshTexts(bool notify)
    {
        std::string text;
        std::vector<Target> targets;
        const bool haveFile = mpc::readTextFile(mpc::contentPath(kConfigName), text);
        if (haveFile) parseConfig(text, targets);

        const int idx = getInt(kTarget) - 1;
        std::string name, source, dest;
        if (!haveFile)            { name = "No config";  source = "Content/foldersync.conf fehlt"; dest = "-"; }
        else if (targets.empty()) { name = "No target";  source = "foldersync.conf hat keinen [Name]-Block"; dest = "-"; }
        else if (idx < 0 || idx >= (int)targets.size())
                                  { name = "-";          source = "-"; dest = "Dieses Target ist nicht in der Konfiguration"; }
        else
        {
            const Target& t = targets[(size_t)idx];
            name   = t.name.empty() ? std::string("-") : t.name;
            source = t.source.empty() ? std::string("-") : t.source;
            dest   = t.host.empty() ? std::string("-")
                                    : ((t.user.empty() ? std::string() : t.user + "@") + t.host + ":" + t.dest);
        }

        name = fitText(name); source = fitText(source); dest = fitText(dest);

        // Jobliste: eine Zeile pro Ziel, das gewählte ist mit ">" markiert. Unbenutzte Zeilen bleiben leer.
        // Der aktive Job steht im Parameter "Active Job", alle anderen in "Job": so kann die Oberfläche ihn farbig zeigen.
        std::string jobs[kMaxTargets], jobsActive[kMaxTargets], names[kMaxTargets];
        for (int i = 0; i < kMaxTargets; ++i)
        {
            jobs[i] = " "; jobsActive[i] = " ";
            if (haveFile && i < (int)targets.size())
            {
                const std::string& n = targets[(size_t)i].name;
                names[i] = n;
                const bool active = (i == idx);
                const std::string line = fitHead(std::string(active ? "> " : "   ") + std::to_string(i + 1) + "  " + (n.empty() ? std::string("(no name)") : n), 23);
                (active ? jobsActive[i] : jobs[i]) = line;
            }
            else if (i == 0 && !haveFile) jobs[i] = "   (no config)";
        }

        bool changed;
        {
            std::lock_guard<std::mutex> lock(textMutex_);
            changed = (name != txtName_) || (source != txtSource_) || (dest != txtDest_);
            for (int i = 0; i < kMaxTargets; ++i)
            {
                if (jobs[i] != txtJobs_[i] || jobsActive[i] != txtJobsActive_[i] || names[i] != txtNames_[i]) changed = true;
                txtJobs_[i] = jobs[i]; txtJobsActive_[i] = jobsActive[i]; txtNames_[i] = names[i];
            }
            txtName_ = name; txtSource_ = source; txtDest_ = dest;
        }
        if (!notify || !changed) return;

        // Der Wert ändert sich nur, damit die MPC den Anzeigetext neu abfragt.
        textTick_ = (textTick_ + 1) % 100;
        const float v = (float)textTick_ / 100.0f;
        set(kSource, v); set(kDest, v); set(kTargetName, v);
        notifyHostParameterChanged(kSource);
        notifyHostParameterChanged(kDest);
        notifyHostParameterChanged(kTargetName);
        for (int i = 0; i < kMaxTargets; ++i)
        {
            set(kJob1 + i, v); notifyHostParameterChanged(kJob1 + i);
            set(kJobA1 + i, v); notifyHostParameterChanged(kJobA1 + i);
        }
        notifyHostParameterChanged(kTarget);   // Anzeige "2  Name" neu lesen
        requestDisplayUpdate();
    }

#ifdef FOLDERSYNC_CAN_SPAWN
    static void* threadEntry(void* self)
    {
        static_cast<MPCFolderSync*>(self)->workerLoop();
        return nullptr;
    }

    // Eigener Thread: wartet auf das Start-Flag (alle 100 ms nachsehen, praktisch keine Last).
    void workerLoop()
    {
        while (!stop_.load())
        {
            if (startPending_.exchange(false))
            {
                running_.store(true);
                setStatus(kStRunning);
                const int result = doSync();
                set(kStart, 0.0f);
                notifyHostParameterChanged(kStart);
                setStatus(result);
                running_.store(false);
                needRefresh_.store(true);
            }
            else
            {
                const int cur = getInt(kTarget);
                if (cur != lastTarget_) { lastTarget_ = cur; needRefresh_.store(true); }
                if (needRefresh_.exchange(false)) refreshTexts(true);
                usleep(100000);
            }
        }
    }

    // Setzt den Fortschrittstext (nur Worker-Thread) und stößt die MPC an, ihn neu zu lesen.
    void setProgress(const std::string& text)
    {
        {
            std::lock_guard<std::mutex> lock(textMutex_);
            if (text == txtProgress_) return;
            txtProgress_ = text;
        }
        textTick_ = (textTick_ + 1) % 100;
        set(kProgress, (float)textTick_ / 100.0f);
        notifyHostParameterChanged(kProgress);
        notifyHostParameterChanged(kStatus);   // der Status zeigt "Running 45%"
        requestDisplayUpdate();
    }

    // Letzter Prozentwert in der rsync-Ausgabe (Ende der Datei), oder -1.
    static int lastPercent(const std::string& path)
    {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return -1;
        char buf[2048];
        const off_t size = lseek(fd, 0, SEEK_END);
        const off_t start = size > (off_t)sizeof(buf) ? size - (off_t)sizeof(buf) : 0;
        lseek(fd, start, SEEK_SET);
        const ssize_t n = read(fd, buf, sizeof(buf));
        close(fd);
        for (ssize_t i = n - 1; i > 0; --i)
        {
            if (buf[i] != '%' || buf[i - 1] < '0' || buf[i - 1] > '9') continue;
            ssize_t b = i - 1;
            while (b > 0 && buf[b - 1] >= '0' && buf[b - 1] <= '9') --b;
            if (i - b > 3) continue;
            return std::atoi(std::string(buf + b, (size_t)(i - b)).c_str());
        }
        return -1;
    }

    // Hängt die rsync-Ausgabe an das Log an, ohne die vielen Fortschrittszeilen.
    static void appendOutput(const std::string& path, SyncLog& log)
    {
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) return;
        std::string cur;
        auto flush = [&]() {
            if (!cur.empty() && cur.find("to-chk") == std::string::npos && cur.find("xfr#") == std::string::npos)
                log.line("%s", cur.c_str());
            cur.clear();
        };
        for (uint8_t c : bytes) { if (c == '\n' || c == '\r') flush(); else cur.push_back((char)c); }
        flush();
    }

    // Hält den Start-Button leuchtend, solange der Sync läuft (auch wenn jemand ihn währenddessen antippt).
    void keepButtonLit()
    {
        if (getNormalised(kStart) < 0.5f)
        {
            setNormalised(kStart, 1.0f);
            notifyHostParameterChanged(kStart);
        }
    }

    // Übersetzt/kopiert den SSH-Schlüssel für dbclient nach <work>/key. Dbclient versteht nur das
    // Dropbear-Format; Schlüssel von ssh-keygen (-----BEGIN ...) werden mit dropbearconvert übersetzt.
    std::string prepareDropbearKey(const std::string& source, const std::string& multi, const WorkDir& work, SyncLog& log)
    {
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(source, bytes) || bytes.empty())
        {
            log.line("Schluessel kann nicht gelesen werden: %s", source.c_str());
            return std::string();
        }
        const std::string out = work.file("key");
        const bool pem = bytes.size() > 10 && std::memcmp(bytes.data(), "-----BEGIN", 10) == 0;
        if (!pem)
        {
            if (!writeFileMode(out, bytes, 0600)) { log.line("Temporaere Schluesseldatei nicht moeglich (%s)", std::strerror(errno)); return std::string(); }
            return out;
        }

        // Die Eingabe erst nach /tmp (0600) legen: dropbearconvert liest sie von dort.
        const std::string in = work.file("key.in");
        if (!writeFileMode(in, bytes, 0600)) { log.line("Temporaere Schluesseldatei nicht moeglich (%s)", std::strerror(errno)); return std::string(); }
        const std::vector<std::string> args = { multi, "dropbearconvert", "openssh", "dropbear", in, out };
        const int code = runChild(multi, args, log.fd());
        unlink(in.c_str());
        if (code != 0 || access(out.c_str(), R_OK) != 0)
        {
            log.line("Schluessel konnte nicht uebersetzt werden (Code %d). Er darf kein Passwort haben und muss ein ssh-keygen-Schluessel sein (ed25519 empfohlen).", code);
            unlink(out.c_str());
            return std::string();
        }
        chmod(out.c_str(), 0600);
        return out;
    }

    // Führt einen kompletten Sync aus und gibt den Status zurück. Alles Wichtige steht in foldersync.log.
    int doSync()
    {
        spawnErr_ = 0;
        SyncLog log(mpc::contentPath(kLogName));
        {
            time_t now = time(nullptr);
            struct tm tmv;
            char stamp[32] = "?";
            if (localtime_r(&now, &tmv) != nullptr) std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
            log.line("=== %s  Sync gestartet ===", stamp);
        }

        std::string text;
        if (!mpc::readTextFile(mpc::contentPath(kConfigName), text))
        {
            log.line("Konfiguration nicht lesbar: %s", mpc::contentPath(kConfigName).c_str());
            return kStNoConfig;
        }
        std::vector<Target> targets;
        parseConfig(text, targets);
        if (targets.empty())
        {
            log.line("In %s ist noch kein Ziel eingetragen ([Name]-Block).", kConfigName);
            return kStNoConfig;
        }

        const int idx = getInt(kTarget) - 1;
        if (idx < 0 || idx >= (int)targets.size())
        {
            log.line("Target %d ist in der Konfiguration nicht vorhanden (%d Ziele).", idx + 1, (int)targets.size());
            return kStBadTarget;
        }
        const Target& t = targets[(size_t)idx];
        log.line("Ziel %d: %s", idx + 1, t.name.c_str());

        // Eingaben prüfen (verhindert u. a. Optionen, die sich als Host/Ordner tarnen).
        if (!isSafeName(t.host) || (!t.user.empty() && !isSafeName(t.user)) || !isPort(t.port) ||
            t.source.empty() || t.source[0] != '/' || t.dest.empty() ||
            t.key.find('"') != std::string::npos || t.password.find('\0') != std::string::npos || !isSafeRemotePath(t.rsyncPath))
        {
            log.line("Ungueltige Angaben: host/user nur Buchstaben, Ziffern . - _ ; port nur Ziffern; source muss mit / beginnen; dest darf nicht leer sein; rsync_path nur / . _ - + und Buchstaben/Ziffern.");
            return kStBadTarget;
        }

        // Arbeitsordner in /tmp mit den ausführbaren Kopien der mitgelieferten Programme.
        WorkDir work(this);
        if (!work.ok())
        {
            log.line("Arbeitsordner in /tmp kann nicht angelegt werden (%s)", std::strerror(errno));
            return kStFailed;
        }

        std::string rsyncBin = work.stage("rsync", log);
        const bool bundledRsync = !rsyncBin.empty();
        if (!bundledRsync) rsyncBin = findExecutable("rsync");
        if (rsyncBin.empty())
        {
            log.line("rsync wurde nicht gefunden: weder mitgeliefert (Content/%s/rsync) noch im System.", kBinDir);
            logMounts(log);
            return kStNoRsync;
        }
        log.line("rsync: %s", bundledRsync ? "mitgeliefert" : rsyncBin.c_str());

        // SSH-Client: eigener Befehl (ssh_command) > mitgelieferter dbclient > ssh des Systems.
        std::string dropbearMulti;
        std::string sshBin;
        if (t.sshCommand.empty())
        {
            dropbearMulti = work.stage("dropbearmulti", log);
            if (dropbearMulti.empty()) sshBin = findExecutable("ssh");
        }
        else
        {
            const size_t sp = t.sshCommand.find(' ');
            sshBin = findExecutable(t.sshCommand.substr(0, sp));
        }
        const bool useDropbear = !dropbearMulti.empty();
        if (!useDropbear && sshBin.empty())
        {
            log.line("ssh wurde nicht gefunden: nicht mitgeliefert (Content/%s/dropbearmulti) und nicht im System (ggf. ssh_command in der Konfiguration setzen).", kBinDir);
            return kStNoSsh;
        }
        log.line("ssh: %s", useDropbear ? "mitgelieferter dbclient" : sshBin.c_str());

        logMounts(log);

        // Anmeldung: Schlüssel hat Vorrang, sonst Passwort (nur mit dem mitgelieferten dbclient).
        const bool usePassword = useDropbear && t.key.empty() && !t.password.empty();
        if (!t.password.empty() && !useDropbear)
            log.line("Hinweis: 'password' funktioniert nur mit dem mitgelieferten SSH-Client; bitte 'key' verwenden.");
        if (!t.password.empty() && !t.key.empty())
            log.line("Hinweis: 'key' und 'password' sind gesetzt, es wird der Schluessel verwendet.");
        log.line("Anmeldung: %s", usePassword ? "Passwort" : (t.key.empty() ? "ohne Schluessel/Passwort" : "Schluessel"));

        // Schlüssel: relativ = im Content-Ordner.
        std::string keyForSsh;
        if (!t.key.empty())
        {
            const std::string keySource = (t.key[0] == '/') ? t.key : mpc::contentPath(t.key);
            if (useDropbear)
            {
                keyForSsh = prepareDropbearKey(keySource, dropbearMulti, work, log);
                if (keyForSsh.empty()) return spawnErr_ != 0 ? kStNoExec : kStFailed;
            }
            else
            {
                std::vector<uint8_t> bytes;
                if (!mpc::readFile(keySource, bytes) || bytes.empty())
                {
                    log.line("Schluessel kann nicht gelesen werden: %s", keySource.c_str());
                    return kStFailed;
                }
                if (!writeFileMode(work.file("key"), bytes, 0600))
                {
                    log.line("Temporaere Schluesseldatei nicht moeglich (%s)", std::strerror(errno));
                    return kStFailed;
                }
                keyForSsh = work.file("key");
            }
        }

        // Ort der bekannten Host-Schlüssel. dbclient legt sie unter $HOME/.ssh/known_hosts ab.
        const std::string knownHosts = mpc::contentPath("known_hosts");
        std::string homeDir = "/tmp";
        if (useDropbear)
        {
            homeDir = mpc::contentPath("home");
            mkdir(homeDir.c_str(), 0755);
            mkdir((homeDir + "/.ssh").c_str(), 0700);
        }

        auto q = [](const std::string& s) { return "\"" + s + "\""; };
        std::string sshE;
        if (!t.sshCommand.empty())
        {
            sshE = t.sshCommand;
            replaceAll(sshE, "{port}", t.port);
            replaceAll(sshE, "{key}", keyForSsh);
            replaceAll(sshE, "{known_hosts}", knownHosts);
        }
        else if (useDropbear)
        {
            // dbclient: -y = unbekannte Host-Schlüssel merken, geänderte Schlüssel werden abgelehnt.
            sshE = q(dropbearMulti) + " dbclient -p " + t.port + " -y";
            if (!usePassword) sshE += " -o BatchMode=yes";   // BatchMode verbietet die Passwortabfrage
            if (!keyForSsh.empty()) sshE += " -i " + q(keyForSsh);
        }
        else
        {
            sshE = q(sshBin) + " -p " + t.port +
                   " -o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new" +
                   " -o UserKnownHostsFile=" + q(knownHosts);
            if (!keyForSsh.empty()) sshE += " -i " + q(keyForSsh) + " -o IdentitiesOnly=yes";
        }

        const bool dry = getBool(kDryRun);
        const std::string remote = (t.user.empty() ? std::string() : t.user + "@") + t.host + ":" + t.dest;

        // -r -t statt -a: FAT/exFAT-Laufwerke kennen keine Rechte/Besitzer. modify-window wegen 2-s-Zeitstempeln.
        std::vector<std::string> args;
        args.push_back(rsyncBin);
        args.push_back(dry ? "-rtvn" : "-rtv");
        args.push_back("--partial");
        args.push_back("--modify-window=2");
        args.push_back("--timeout=30");
        args.push_back("--stats");
        args.push_back("--info=progress2");   // Gesamtfortschritt in Prozent
        args.push_back("--no-inc-recursive"); // erst alles zählen, damit der Prozentwert nicht springt
        for (const std::string& ex : t.excludes) args.push_back("--exclude=" + ex);
        if (t.del) args.push_back("--delete");
        if (!t.rsyncPath.empty()) args.push_back("--rsync-path=" + t.rsyncPath);
        args.push_back("-e");
        args.push_back(sshE);
        args.push_back(t.source);
        args.push_back(remote);

        log.line("Befehl:");
        for (const std::string& a : args) log.line("  %s", a.c_str());
        if (dry) log.line("(Dry Run: es wird nichts kopiert)");

        if (!t.excludes.empty()) log.line("%d Ausschluss-Muster (exclude)", (int)t.excludes.size());
        // rsync schreibt in eine Datei in /tmp; daraus liest der Fortschritt. Danach kommt sie ins Log.
        const std::string outPath = work.file("out");
        const int outFd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        setProgress(dry ? "-" : "0%");
        const int code = runChild(rsyncBin, args, outFd >= 0 ? outFd : log.fd(), homeDir, /*keepLit=*/true,
                                  usePassword ? t.password : std::string(), outFd >= 0 ? outPath : std::string());
        if (outFd >= 0) { close(outFd); appendOutput(outPath, log); }
        if (!dry && (code == 0 || code == 24)) setProgress("100%");

        if (code == -2) { log.line("Abgebrochen (Plugin wurde entfernt)."); return kStFailed; }
        if (code == -1 && spawnErr_ != 0)
        {
            log.line("rsync konnte nicht gestartet werden (%s).%s", std::strerror(spawnErr_),
                     bundledRsync ? " Ist /tmp auf diesem Geraet 'noexec' eingehaengt?" : "");
            return kStNoExec;
        }
        log.line("rsync beendet mit Code %d", code);
        if (code == 255 || code == 12)
            log.line("Hinweis: Code %d heisst meist, dass die SSH-Verbindung scheiterte (Adresse, Port, Schluessel) oder dass auf dem Ziel kein rsync gefunden wurde (dann rsync_path setzen).", code);
        // 0 = ok, 24 = Dateien waren beim Lesen verschwunden (unkritisch)
        return (code == 0 || code == 24) ? kStDone : kStFailed;
    }

    // Startet ein Programm mit posix_spawn, leitet Ausgabe in die Log-Datei und wartet.
    // Rückgabe: Exit-Code, 128+Signal, -1 bei Startfehler (dann ist spawnErr_ gesetzt), -2 wenn abgebrochen.
    int runChild(const std::string& program, const std::vector<std::string>& args, int logfd,
                 const std::string& homeDir = "/tmp", bool keepLit = false,
                 const std::string& password = std::string(), const std::string& progressPath = std::string())
    {
        std::vector<char*> argv;
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        const std::string envPath = "PATH=" + searchPath();
        const std::string envHome = "HOME=" + homeDir;
        // Das Passwort geht nur per Umgebung an dbclient (nicht in Befehlszeile oder Log).
        const std::string envPass = "DROPBEAR_PASSWORD=" + password;
        char* envp[] = { const_cast<char*>(envPath.c_str()), const_cast<char*>(envHome.c_str()),
                         password.empty() ? nullptr : const_cast<char*>(envPass.c_str()), nullptr };

        const int devIn = open("/dev/null", O_RDONLY);
        const int sink  = logfd >= 0 ? logfd : open("/dev/null", O_WRONLY);

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        if (devIn >= 0) posix_spawn_file_actions_adddup2(&fa, devIn, 0);
        if (sink  >= 0) { posix_spawn_file_actions_adddup2(&fa, sink, 1); posix_spawn_file_actions_adddup2(&fa, sink, 2); }

        pid_t pid = 0;
        const int rc = posix_spawn(&pid, program.c_str(), &fa, nullptr, argv.data(), envp);
        posix_spawn_file_actions_destroy(&fa);
        if (devIn >= 0) close(devIn);
        if (sink >= 0 && logfd < 0) close(sink);
        if (rc != 0) { spawnErr_ = rc; return -1; }

        int status = 0;
        for (;;)
        {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            if (r < 0 && errno != EINTR) return -1;
            if (stop_.load())
            {
                kill(pid, SIGTERM);
                usleep(500000);
                if (waitpid(pid, &status, WNOHANG) != pid) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
                return -2;
            }
            if (keepLit) keepButtonLit();
            if (!progressPath.empty())
            {
                const int pc = lastPercent(progressPath);
                if (pc >= 0) setProgress(std::to_string(pc) + "%");
            }
            usleep(200000);
        }
        if (WIFEXITED(status))   return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }

    pthread_t         thread_{};
    bool              threadStarted_ = false;
    int               spawnErr_ = 0;       // nur vom Worker-Thread benutzt
#else
    int doSync() { return kStNoRsync; }
#endif

    std::atomic<bool> stop_{false};
    std::atomic<bool> startPending_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> needRefresh_{false};
    int               lastTarget_ = 1;     // nur vom Worker-Thread benutzt
    unsigned          textTick_ = 0;       // nur vom Worker-Thread benutzt

    std::mutex        textMutex_;
    std::string       txtName_, txtSource_, txtDest_;
    std::string       txtJobs_[kMaxTargets], txtJobsActive_[kMaxTargets], txtNames_[kMaxTargets];
    std::string       txtProgress_ = "-";
};

// Erzeugt den exportierten Einstiegspunkt VSTPluginMain. Muss ganz unten stehen.
MPC_PLUGIN_ENTRY(MPCFolderSync)
