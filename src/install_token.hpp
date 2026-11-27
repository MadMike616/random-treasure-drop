#pragma once
// Plugin-local installation state. Commit under the shared mod-install mutex,
// after every operation in this setup component has succeeded.
#include <D2RLPlugin/context.h>
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace InstallToken {
namespace fs = std::filesystem;
inline std::string_view Trim(std::string_view s) {
    const auto a=s.find_first_not_of(" \t\r\n");
    return a==s.npos ? std::string_view{} : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline std::string_view Header(std::string_view line) {
    line=Trim(line.substr(0,line.find('#')));
    return line.size()>=2 && line.front()=='[' && line.back()==']'
        ? Trim(line.substr(1,line.size()-2)) : std::string_view{};
}
inline fs::path Native(fs::path path) {
    path=fs::absolute(path).lexically_normal().make_preferred();
    const auto name=path.native();
    if (name.starts_with(L"\\\\?\\")) return path;
    if (name.starts_with(L"\\\\")) return L"\\\\?\\UNC\\"+name.substr(2);
    return L"\\\\?\\"+name;
}
inline std::optional<std::string> Read(const fs::path& path) {
    if (!fs::exists(Native(path))) return {};
    std::ifstream in(Native(path),std::ios::binary|std::ios::ate);
    const auto size=in.tellg();
    if (!in || size<0 || size>1024*1024) throw std::runtime_error("Cannot read installation TOML");
    std::string text(static_cast<std::size_t>(size),'\0');
    in.seekg(0);
    if (!in.read(text.data(),static_cast<std::streamsize>(text.size())))
        throw std::runtime_error("Cannot read installation TOML completely");
    return text;
}
inline fs::path Config(const D2RL::PluginContext& ctx) {
    if (!ctx.pluginConfigPath || !*ctx.pluginConfigPath)
        throw std::runtime_error("Loader did not supply an installation TOML path");
    return fs::path(ctx.pluginConfigPath);
}
inline std::optional<std::string> Value(std::string_view text,std::string_view section,std::string_view key) {
    std::optional<std::string> result;
    bool owned=false,seenSection=false;
    while (!text.empty()) {
        const auto end=text.find('\n');
        auto line=Trim(text.substr(0,end));
        if (line.starts_with("\xef\xbb\xbf")) line.remove_prefix(3);
        const auto head=Header(line);
        if (!head.empty()) {
            owned=head==section;
            if (owned && seenSection) throw std::runtime_error("Duplicate installation section");
            seenSection=seenSection||owned;
        } else if (owned) {
            const auto eq=line.find('=');
            if (eq!=line.npos && Trim(line.substr(0,eq))==key) {
                if (result) throw std::runtime_error("Duplicate installation token field");
                auto value=line.substr(eq+1);
                result=std::string(Trim(value.substr(0,value.find('#'))));
            }
        }
        if (end==text.npos) break;
        text.remove_prefix(end+1);
    }
    return result;
}
inline std::optional<std::string> Get(const D2RL::PluginContext& ctx,std::string_view key,
        std::string_view section="installation") {
    return Value(Read(Config(ctx)).value_or(""),section,key);
}
inline bool Complete(const D2RL::PluginContext& ctx,std::string_view section="installation") {
    const auto text=Read(Config(ctx)).value_or("");
    const auto value=Value(text,section,"complete");
    if (!value || *value=="false") return false;
    if (*value!="true" || Value(text,section,"schema_version")!=std::optional<std::string>("1"))
        throw std::runtime_error("Invalid installation token; set complete = false in its installation section to retry");
    return true;
}
inline std::string Set(std::string text,std::string_view section,std::string_view key,std::string_view value) {
    (void)Value(text,section,key);
    const std::string newline=text.find("\r\n")==text.npos ? "\n" : "\r\n";
    bool owned=false,found=false;
    std::size_t insert=text.size();
    for (std::size_t pos=0;pos<text.size();) {
        const auto end=text.find('\n',pos);
        const auto count=end==text.npos ? text.size()-pos : end-pos+1;
        const auto line=std::string_view(text).substr(pos,count);
        const auto head=Header(line);
        if (!head.empty()) {
            if (owned) {insert=pos;break;}
            owned=head==section;found=found||owned;
        } else if (owned) {
            const auto eq=line.find('=');
            if (eq!=line.npos && Trim(line.substr(0,eq))==key) {
                text.replace(pos,count,std::string(key)+" = "+std::string(value)+newline);
                return text;
            }
        }
        pos+=count;
    }
    std::string addition;
    if (!found) addition="["+std::string(section)+"]"+newline;
    addition+=std::string(key)+" = "+std::string(value)+newline;
    if (insert && text[insert-1]!='\n') addition=newline+addition;
    text.insert(insert,addition);
    return text;
}
inline void Atomic(const fs::path& path,const std::string& text) {
    fs::create_directories(Native(path.parent_path()));
    static std::atomic_uint64_t sequence{};
    auto temporary=path;
    temporary+=L".install-token-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++sequence)+L".tmp";
    const auto handle=CreateFileW(Native(temporary).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot stage installation token");
    DWORD written{};
    const bool okay=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr)
        && written==text.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!okay || !MoveFileExW(Native(temporary).c_str(),Native(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(Native(temporary).c_str());throw std::runtime_error("Cannot save installation token");
    }
}
inline void Commit(const D2RL::PluginContext& ctx,
        const std::vector<std::pair<std::string,std::string>>& fields={},
        std::string_view section="installation",std::string_view defaults={}) {
    const auto path=Config(ctx);
    const auto old=Read(path);
    const auto update=[&](std::string text) {
        for (const auto& [key,value]:fields) text=Set(std::move(text),section,key,value);
        text=Set(std::move(text),section,"schema_version","1");
        return Set(std::move(text),section,"complete","true");
    };
    auto initial=old.value_or(std::string(defaults));
    if (!old && defaults.empty()) {
        // Retain loader-provided/embedded defaults when the TOML is not on disk yet.
        std::vector<char> buffer(64*1024,'\0');
        std::uint32_t required{};
        if (ctx.ReadConfig(buffer.data(),static_cast<std::uint32_t>(buffer.size()),&required)
                && required<=buffer.size()) {
            buffer.back()='\0';
            initial=buffer.data();
        }
    }
    const auto after=update(initial);
    std::optional<fs::path> mirror;
    std::optional<std::string> previousMirror;
    const auto loader=path.parent_path().parent_path();
    const auto mod=loader.parent_path();
    if (_wcsicmp(loader.filename().c_str(),L"d2rloader")==0
            && _wcsicmp(mod.parent_path().filename().c_str(),L"mods")==0) {
        const auto base=mod.parent_path().parent_path()/L"d2rloader/I Hate TOMLs";
        if (fs::is_directory(base)) {
            mirror=base/mod.filename()/path.filename();previousMirror=Read(*mirror);
        }
    }
    bool attempted=false;
    try {
        if (mirror) Atomic(*mirror,update(previousMirror.value_or(initial)));
        attempted=true;
        if (!ctx.WriteConfig(after.c_str()) || Read(path)!=std::optional<std::string>(after))
            throw std::runtime_error("Loader could not commit installation token");
    } catch (...) {
        if (attempted) {
            (void)ctx.WriteConfig(old.value_or(std::string(defaults)).c_str());
            if (!old) {std::error_code ignored;fs::remove(path,ignored);}
        }
        if (mirror) {
            try {
                if (previousMirror) Atomic(*mirror,*previousMirror);
                else {std::error_code ignored;fs::remove(*mirror,ignored);}
            } catch (...) {throw std::runtime_error("Installation token failed and its TOML mirror could not be restored");}
        }
        throw;
    }
}
inline std::string WithoutSections(std::string_view text) {
    std::string out;bool skip=false;
    while (!text.empty()) {
        const auto end=text.find('\n');
        const auto count=end==text.npos ? text.size() : end+1;
        const auto line=text.substr(0,count);
        const auto head=Header(line);
        if (!head.empty()) skip=head=="installation" || head.starts_with("installation.");
        if (!skip) out+=line;
        else if (end!=text.npos) out+='\n';
        text.remove_prefix(count);
    }
    return out;
}
class Lock {
    HANDLE handle{};
public:
    Lock() {
        handle=CreateMutexW(nullptr,FALSE,L"Local\\D2RLoader.ModFirstRunInstall.v1");
        if (!handle) throw std::runtime_error("Cannot open shared installation lock");
        const auto result=WaitForSingleObject(handle,10000);
        if (result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED) {
            CloseHandle(handle);handle=nullptr;
            throw std::runtime_error("Another installer holds the shared lock; retry next launch");
        }
    }
    ~Lock() {if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
    Lock(const Lock&)=delete;
    Lock& operator=(const Lock&)=delete;
};
} // namespace InstallToken
