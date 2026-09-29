
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <filesystem>
#include <openssl/evp.h>

namespace fs = std::filesystem;
using namespace std;


namespace LogFormat {
    constexpr int HASH_LEN     = 40;
    constexpr int HASH_OFFSET  = 7;                       // after "commit "
    constexpr int HEAD_OFFSET  = HASH_OFFSET + HASH_LEN + 1; // one space after hash
    constexpr int HEAD_LEN     = 4;                        // "HEAD" or spaces
    constexpr int MSG_OFFSET   = HEAD_OFFSET + HEAD_LEN + 1;
}

class WtGit {
public:
    WtGit();

    void setRoot(const string &path);
    void printHelp(const string &topic) const;

    void init();
    void add(const string &path);
    void commit(const string &message);
    void printLog(const string &flag) const;
    void checkout(const string &hash);
    void revert(const string &hash);
    void status() const;
    void resolve();

    bool hasUnresolvedConflict() const;

private:
    string root_;

    // --- path / filesystem helpers ---------------------------------
    string entryType(const string &path) const;     // "file" | "directory" | ""
    string relativePath(const string &path) const;
    bool isIgnored(const string &path) const;
    bool filesEqual(const string &a, const string &b) const;

    // --- repo bookkeeping --------------------------------------------
    bool isInitialized() const;
    bool requireRepo() const;     
    bool hasStagedChanges() const;

    void addToAddLog(const string &path);
    void addToStagingCache(const string &path);
    void clearStagingArea();

    string newCommitHash(const string &message) const;
    string readHeadHash() const;
    void setConflictFlag(bool active);

    static constexpr int BUFFER_SIZE = 8192;
};

// =====================================================================
// construction / setup
// =====================================================================

WtGit::WtGit()
{
    const char *envDir = getenv("dir");
    if (!envDir)
    {
        cerr << "ERROR. Environment variable \"dir\" is not set.\n";
        exit(1);
    }
    root_ = envDir;
}

void WtGit::setRoot(const string &path)
{
    root_ += "/" + path;
    error_code ec;
    fs::create_directories(root_, ec);
    if (ec)
        cerr << "ERROR. Could not create root directory: " << ec.message() << "\n";
}

void WtGit::printHelp(const string &topic) const
{
    bool all = topic.empty();
    if (all)
        cout << "Usage: wtgit [--help <topic>] [init] [add <paths>] [commit \"<message>\"] "
                "[log] [status] [checkout <hash>] [revert <hash>] [resolve]\n";
    if (all || topic == "init")
        cout << "  init                 Initialize an empty repository in the current "
                "(or a given) directory.\n";
    if (all || topic == "add")
        cout << "  add <paths...>       Stage files or directories. Use \"add .\" to stage everything.\n";
    if (all || topic == "commit")
        cout << "  commit \"<message>\"   Record the currently staged changes.\n";
    if (all || topic == "log")
        cout << "  log [--oneline]      Show the commit history.\n";
    if (all || topic == "status")
        cout << "  status               Show staged and unstaged changes.\n";
    if (all || topic == "checkout")
        cout << "  checkout <hash>      Restore the working tree to a given commit.\n";
    if (all || topic == "revert")
        cout << "  revert <hash>        Undo a given commit, flagging conflicts if needed.\n";
    if (all || topic == "resolve")
        cout << "  resolve              Clear a pending merge-conflict lock so commits can resume.\n";
}

// =====================================================================
// low-level helpers
// =====================================================================

string WtGit::entryType(const string &path) const
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0)
        return "";
    if (S_ISDIR(info.st_mode)) return "directory";
    if (S_ISREG(info.st_mode)) return "file";
    return "";
}

string WtGit::relativePath(const string &path) const
{
    size_t len = root_.size();
    if (path.substr(0, len) == root_)
        return path.substr(len + 1);
    return path;
}

bool WtGit::isIgnored(const string &path) const
{
    ifstream ignoreFile((root_ + "/.wtgitignore").c_str());
    string rel = relativePath(path);
    string line;
    while (getline(ignoreFile, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] != '/')
            continue;
        string pattern = line.substr(1); // drop leading '/'

        // exact match, or rel is inside an ignored directory
        // (".wtgit" must also ignore ".wtgit/add.log", etc.)
        if (rel == pattern)
            return true;
        if (rel.size() > pattern.size() &&
            rel.compare(0, pattern.size(), pattern) == 0 &&
            rel[pattern.size()] == '/')
            return true;
    }
    return false;
}

bool WtGit::filesEqual(const string &a, const string &b) const
{
    string typeA = entryType(a), typeB = entryType(b);
    if (typeA == "directory" && typeB == "directory")
        return true;
    if (typeA != "file" || typeB != "file")
        return false;

    ifstream fa(a, ios::binary | ios::ate);
    ifstream fb(b, ios::binary | ios::ate);
    if (!fa.is_open() || !fb.is_open())
        return false;
    if (fa.tellg() != fb.tellg())
        return false;

    fa.seekg(0);
    fb.seekg(0);
    vector<char> bufA(BUFFER_SIZE), bufB(BUFFER_SIZE);
    while (fa && fb)
    {
        fa.read(bufA.data(), BUFFER_SIZE);
        fb.read(bufB.data(), BUFFER_SIZE);
        streamsize n = fa.gcount();
        if (n != fb.gcount())
            return false;
        if (n > 0 && memcmp(bufA.data(), bufB.data(), static_cast<size_t>(n)) != 0)
            return false;
        if (n < BUFFER_SIZE)
            break;
    }
    return true;
}

// =====================================================================
// repo state / safeguards
// =====================================================================

bool WtGit::isInitialized() const
{
    if (entryType(root_ + "/.wtgit") != "directory") return false;
    if (entryType(root_ + "/.wtgit/add.log") != "file") return false;
    if (entryType(root_ + "/.wtgit/commit.log") != "file") return false;
    if (entryType(root_ + "/.wtgitignore") != "file") return false;
    if (entryType(root_ + "/.wtgit/conflict") != "file") return false;
    return true;
}

bool WtGit::requireRepo() const
{
    if (!isInitialized())
    {
        cout << "ERROR. Repository not initialized properly.\n";
        cout << "Try \"wtgit init\".\n";
        return false;
    }
    return true;
}

bool WtGit::hasUnresolvedConflict() const
{
    ifstream fin((root_ + "/.wtgit/conflict").c_str());
    string line;
    getline(fin, line);
    return line == "true";
}

void WtGit::setConflictFlag(bool active)
{
    ofstream fout((root_ + "/.wtgit/conflict").c_str(), ofstream::trunc);
    fout << (active ? "true" : "false") << "\n";
}

void WtGit::resolve()
{
    setConflictFlag(false);
    cout << "Merge conflict cleared. You can make commits again.\n";
}

bool WtGit::hasStagedChanges() const
{
    return entryType(root_ + "/.wtgit/.add") == "directory";
}

// =====================================================================
// init
// =====================================================================

void WtGit::init()
{
    if (entryType(root_ + "/.wtgit") == "directory")
    {
        cout << "Repository has already been initialized.\n";
        return;
    }

    ofstream ignore((root_ + "/.wtgitignore").c_str());
    ignore << "/.gitignore\n/.wtgit\n/.git\n/.wtgitignore\n/.node_modules\n/.env\n";
    ignore.close();

    error_code ec;
    fs::create_directories(root_ + "/.wtgit", ec);
    if (ec)
    {
        cout << "ERROR. Failed to initialize a repository: " << ec.message() << "\n";
        return;
    }

    ofstream(root_ + "/.wtgit/commit.log").close();
    ofstream(root_ + "/.wtgit/add.log").close();
    ofstream conflict(root_ + "/.wtgit/conflict");
    conflict << "false\n";
    conflict.close();

    cout << "Initialized an empty wtgit repository at " << root_ << "\n";
}

// =====================================================================
// add
// =====================================================================

void WtGit::addToAddLog(const string &path)
{
    {
        ifstream fin((root_ + "/.wtgit/add.log").c_str());
        string check;
        while (getline(fin, check))
            if (check == path)
                return;
    }
    ofstream fout((root_ + "/.wtgit/add.log").c_str(), ofstream::app);
    fout << path << "\n";
}

void WtGit::addToStagingCache(const string &path)
{
    if (entryType(root_ + "/.wtgit/.add") != "directory")
        fs::create_directories(root_ + "/.wtgit/.add");

    fs::path dest = root_ + "/.wtgit/.add/" + relativePath(path);
    if (entryType(path) == "directory")
    {
        if (entryType(dest.string()) != "directory")
            fs::create_directories(dest);
    }
    else
    {
        fs::create_directories(dest.parent_path());
        fs::copy_file(path, dest, fs::copy_options::overwrite_existing);
    }
}

void WtGit::add(const string &path)
{
    if (!requireRepo()) return;
    if (hasUnresolvedConflict())
    {
        cout << "You have unresolved merge conflicts. Run \"wtgit resolve\" first.\n";
        return;
    }

    string fullPath = (path == ".") ? root_ : root_ + "/" + path;
    string type = entryType(fullPath);

    if (type.empty())
    {
        cout << "ERROR. \"" << path << "\" does not exist.\n";
        return;
    }
    if (isIgnored(fullPath))
        return;

    if (type == "directory")
    {
        addToAddLog(relativePath(fullPath).empty() ? "." : relativePath(fullPath));
        addToStagingCache(fullPath);
        cout << "Added Directory : \"" << path << "\"\n";

        for (auto &entry : fs::recursive_directory_iterator(fullPath))
        {
            if (isIgnored(entry.path().string()))
                continue;
            string childType = entryType(entry.path().string());
            string rel = relativePath(entry.path().string());
            if (childType == "directory")
            {
                addToAddLog(rel);
                addToStagingCache(entry.path().string());
                cout << "Added Directory : " << entry.path().string() << "\n";
            }
            else if (childType == "file")
            {
                addToAddLog(rel);
                addToStagingCache(entry.path().string());
                cout << "Added File : " << entry.path().string() << "\n";
            }
        }
    }
    else
    { // file
        addToAddLog(relativePath(fullPath));
        addToStagingCache(fullPath);
        cout << "Added File : \"" << path << "\"\n";
    }
}

void WtGit::clearStagingArea()
{
    fs::remove_all(root_ + "/.wtgit/.add");
    ofstream(root_ + "/.wtgit/add.log", ofstream::trunc).close();
}

// =====================================================================
// commit
// =====================================================================

string WtGit::newCommitHash(const string &message) const
{
    uint64_t now = static_cast<uint64_t>(time(nullptr));

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, &now, sizeof(now));
    EVP_DigestUpdate(ctx, message.data(), message.size());
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    ostringstream oss;
    for (unsigned int i = 0; i < digestLen; ++i)
        oss << hex << setfill('0') << setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

string WtGit::readHeadHash() const
{
    ifstream fin((root_ + "/.wtgit/commit.log").c_str());
    string line;
    if (getline(fin, line) && line.size() >= LogFormat::HASH_OFFSET + LogFormat::HASH_LEN)
        return line.substr(LogFormat::HASH_OFFSET, LogFormat::HASH_LEN);
    return "";
}

void WtGit::commit(const string &message)
{
    if (!requireRepo()) return;
    if (hasUnresolvedConflict())
    {
        cout << "You have unresolved merge conflicts. Run \"wtgit resolve\" first.\n";
        return;
    }
    if (!hasStagedChanges())
    {
        cout << "ERROR. Nothing to commit.\n";
        cout << "Try \"wtgit add <path>\".\n";
        return;
    }
    if (message.empty())
    {
        cout << "Please provide a non-empty commit message.\n";
        return;
    }

    string hash = newCommitHash(message);
    string prevHash = readHeadHash();
    string commitDir = root_ + "/.wtgit/.commit/" + hash;
    fs::create_directories(commitDir);

    if (!prevHash.empty())
        fs::copy(root_ + "/.wtgit/.commit/" + prevHash, commitDir,
                 fs::copy_options::recursive | fs::copy_options::skip_existing);
    fs::copy(root_ + "/.wtgit/.add", commitDir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);


    ostringstream rewritten;
    rewritten << "commit " << hash << " -- " << message << " --> HEAD\n";
    {
        ifstream fin((root_ + "/.wtgit/commit.log").c_str());
        string line;
        bool first = true;
        while (getline(fin, line))
        {
            if (first)
            {
                // strip the previous "--> HEAD" suffix from the old top entry
                size_t pos = line.rfind(" --> HEAD");
                if (pos != string::npos)
                    line = line.substr(0, pos);
                first = false;
            }
            rewritten << line << "\n";
        }
    }
    ofstream fout((root_ + "/.wtgit/commit.log").c_str(), ofstream::trunc);
    fout << rewritten.str();
    fout.close();

    clearStagingArea();
    cout << "Committed as " << hash << "\n";
}

// =====================================================================
// log
// =====================================================================

void WtGit::printLog(const string &flag) const
{
    if (!requireRepo()) return;
    if (entryType(root_ + "/.wtgit/.commit") != "directory")
    {
        cout << "No commits yet. Try \"wtgit commit\" first.\n";
        return;
    }

    ifstream fin((root_ + "/.wtgit/commit.log").c_str());
    string line;
    while (getline(fin, line))
    {
        if (flag == "--oneline" && line.size() >= LogFormat::HASH_OFFSET + 7)
        {
            cout << line.substr(LogFormat::HASH_OFFSET, 7) << "  ";
            size_t msgStart = line.find("-- ");
            size_t msgEnd = line.rfind(" -->");
            if (msgStart != string::npos && msgEnd != string::npos && msgEnd > msgStart)
                cout << line.substr(msgStart + 3, msgEnd - (msgStart + 3));
            cout << "\n";
        }
        else
        {
            cout << line << "\n";
        }
    }
}

// =====================================================================
// checkout
// =====================================================================

void WtGit::checkout(const string &hash)
{
    if (!requireRepo()) return;
    if (hasUnresolvedConflict())
    {
        cout << "You have unresolved merge conflicts. Run \"wtgit resolve\" first.\n";
        return;
    }

    string commitDir = root_ + "/.wtgit/.commit/" + hash;
    if (entryType(commitDir) != "directory")
    {
        cout << "Given commit hash doesn't exist.\n";
        cout << "Try \"wtgit log\" and copy the correct commit hash.\n";
        return;
    }

    for (auto &entry : fs::recursive_directory_iterator(commitDir))
    {
        // rel must be computed relative to commitDir (e.g. "sub/b.txt"),
        // NOT relativePath() which strips root_ and would leave the
        // ".wtgit/.commit/<hash>/" prefix still attached, causing the
        // destination to alias the source file itself.
        string rel = entry.path().string().substr(commitDir.size());
        if (entryType(entry.path().string()) == "directory")
            fs::create_directories(root_ + rel);
        else
        {
            fs::path dest = root_ + rel;
            fs::create_directories(dest.parent_path());
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing);
        }
    }
    cout << "Checked out commit " << hash << ".\n";
}

// =====================================================================
// status
// =====================================================================

void WtGit::status() const
{
    if (!requireRepo()) return;

    string headHash = readHeadHash();
    vector<string> staged, notStaged;

    if (entryType(root_ + "/.wtgit/add.log") == "file")
    {
        ifstream fin((root_ + "/.wtgit/add.log").c_str());
        string rel;
        while (getline(fin, rel))
        {
            string stagedCopy = root_ + "/.wtgit/.add/" + rel;
            string committedCopy = root_ + "/.wtgit/.commit/" + headHash + "/" + rel;
            string workingCopy = root_ + "/" + rel;

            if (entryType(stagedCopy) != "file")
                continue;

            if (entryType(committedCopy).empty())
                staged.push_back("created: " + rel);
            else if (!filesEqual(committedCopy, stagedCopy))
                staged.push_back("modified: " + rel);

            if (!filesEqual(workingCopy, stagedCopy))
                notStaged.push_back("modified: " + rel);
        }
    }

    auto alreadyListed = [&](const string &entry) {
        return find(staged.begin(), staged.end(), entry) != staged.end();
    };

    bool hasCommits = entryType(root_ + "/.wtgit/.commit") == "directory";
    for (auto &entry : fs::recursive_directory_iterator(root_))
    {
        if (isIgnored(entry.path().string()) || entryType(entry.path().string()) != "file")
            continue;
        string rel = relativePath(entry.path().string());
        string committedCopy = hasCommits ? root_ + "/.wtgit/.commit/" + headHash + "/" + rel : "";

        if (!hasCommits || entryType(committedCopy).empty())
        {
            if (!alreadyListed("created: " + rel) && !alreadyListed("modified: " + rel))
                notStaged.push_back("created: " + rel);
        }
        else if (!filesEqual(entry.path().string(), committedCopy))
            notStaged.push_back("modified: " + rel);
    }

    sort(staged.begin(), staged.end());
    sort(notStaged.begin(), notStaged.end());
    staged.erase(unique(staged.begin(), staged.end()), staged.end());
    notStaged.erase(unique(notStaged.begin(), notStaged.end()), notStaged.end());

    if (!staged.empty())
    {
        cout << "\nChanges staged for the next commit:\n";
        for (auto &s : staged) cout << "  " << s << "\n";
    }
    if (!notStaged.empty())
    {
        cout << "\nChanges not staged for the next commit:\n";
        for (auto &s : notStaged) cout << "  " << s << "\n";
    }
    if (staged.empty() && notStaged.empty())
        cout << "Everything is up to date.\n";
}

// =====================================================================
// revert
// =====================================================================

void WtGit::revert(const string &hash)
{
    if (!requireRepo()) return;
    if (hasUnresolvedConflict())
    {
        cout << "You have unresolved merge conflicts. Run \"wtgit resolve\" first.\n";
        return;
    }
    if (hasStagedChanges())
    {
        cout << "You still have staged changes that need to be committed first.\n";
        return;
    }

    string commitDir = root_ + "/.wtgit/.commit/" + hash;
    if (entryType(commitDir) != "directory")
    {
        cout << "ERROR. Invalid commit hash provided.\n";
        return;
    }

    bool conflictFound = false;
    for (auto &entry : fs::recursive_directory_iterator(commitDir))
    {
        if (entryType(entry.path().string()) != "file")
            continue;
        string rel = entry.path().string().substr(commitDir.size());
        string workingCopy = root_ + rel;

        if (entryType(workingCopy) == "file" && !filesEqual(workingCopy, entry.path().string()))
        {
            ofstream fout(workingCopy, ofstream::app);
            fout << "\n\nYOUR CHANGES__________<<<<<<<<<<<<<<<<<<<<<\n";
            fout << "______INCOMING (REVERTED) CHANGES>>>>>>>>>>>>>>>>>>>>>\n\n";
            ifstream fin(entry.path().string());
            fout << fin.rdbuf();
            cout << "MERGE CONFLICT IN : " << workingCopy << "\n";
            conflictFound = true;
        }
        else if (entryType(workingCopy) == "file")
            fs::remove(workingCopy);
    }

    if (conflictFound)
    {
        setConflictFlag(true);
        cout << "Revert finished with conflicts. Resolve them, then run \"wtgit resolve\".\n";
    }
    else
        cout << "Reverted commit " << hash << " cleanly.\n";
}

// =====================================================================
// main
// =====================================================================

int main(int argc, char **argv)
{
    WtGit repo;

    if (argc < 2)
    {
        repo.printHelp("");
        return 0;
    }

    string command = argv[1];

    if (command == "--help")
    {
        repo.printHelp(argc >= 3 ? argv[2] : "");
        return 0;
    }

    if (command == "resolve")
    {
        repo.resolve();
        return 0;
    }

    // Every other command is blocked while a merge conflict is pending —
    // this is the safeguard carried over from the functional prototype.
    if (repo.hasUnresolvedConflict())
    {
        cout << "You have unresolved merge conflicts. Run \"wtgit resolve\" first.\n";
        return 1;
    }

    if (command == "init")
    {
        if (argc == 3) repo.setRoot(argv[2]);
        else if (argc > 3)
        {
            cout << "ERROR. Too many arguments.\nDid you mean \"wtgit init\" or \"wtgit init <dir>\"?\n";
            return 1;
        }
        repo.init();
    }
    else if (command == "add")
    {
        if (argc < 3)
        {
            cout << "Please specify at least one path to add.\n";
            return 1;
        }
        for (int i = 2; i < argc; ++i)
            repo.add(argv[i]);
    }
    else if (command == "commit")
    {
        if (argc < 3 || strlen(argv[2]) == 0)
        {
            cout << "Please provide a commit message.\n";
            return 1;
        }
        string message;
        for (int i = 2; i < argc; ++i)
        {
            message += argv[i];
            if (i != argc - 1) message += " ";
        }
        repo.commit(message);
    }
    else if (command == "log")
    {
        if (argc == 2) repo.printLog("");
        else if (argc == 3 && string(argv[2]) == "--oneline") repo.printLog("--oneline");
        else
        {
            cout << "ERROR. Unrecognized flag.\n";
            repo.printHelp("log");
            return 1;
        }
    }
    else if (command == "checkout")
    {
        if (argc != 3)
        {
            cout << "ERROR. Please enter a proper commit hash.\n";
            return 1;
        }
        repo.checkout(argv[2]);
    }
    else if (command == "status")
    {
        if (argc != 2)
        {
            cout << "ERROR. \"wtgit status\" takes no arguments.\n";
            return 1;
        }
        repo.status();
    }
    else if (command == "revert")
    {
        if (argc != 3)
        {
            cout << "ERROR. Please enter a proper commit hash.\n";
            return 1;
        }
        repo.revert(argv[2]);
    }
    else
    {
        cout << "ERROR. Command not recognized: \"" << command << "\"\n";
        repo.printHelp("");
        return 1;
    }

    return 0;
}
