#ifndef CLANG_UTILS_TRANSLATION_UNIT_H
#define CLANG_UTILS_TRANSLATION_UNIT_H


#include <clang-c/Index.h>
#include <iostream>
#include <fstream>
#include <set>
#include <memory>
#include <future>
#include <mutex>
#include <iterator>
#include <algorithm>
#include <unordered_map>
#include <cstring>

#include "complete.h"

// std::ofstream dump_log("/home/paul/clang_log");
// #define DUMP(x) dump_log << std::string(__PRETTY_FUNCTION__) << ": " << #x << " = " << x << std::endl;

// An improved async, that doesn't block
template< class Function, class... Args>
std::future<typename std::result_of<Function(Args...)>::type>
detach_async( Function&& f, Args&&... args )
{
    typedef typename std::result_of<Function(Args...)>::type result_type;
    std::packaged_task<result_type(Args...)> task(std::forward<Function>(f));
    auto fut = task.get_future(); 
    std::thread(std::move(task)).detach();
    return std::move(fut);
}

inline bool starts_with(const char *str, const char *pre)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? false : strncmp(pre, str, lenpre) == 0;
}

class translation_unit
{
    CXIndex index;
    CXTranslationUnit tu;
    const char * filename;
    std::mutex m;

    CXUnsavedFile unsaved_buffer(const char * buffer, unsigned len)
    {
        CXUnsavedFile result;
        result.Filename = this->filename;
        result.Contents = buffer;
        result.Length = len;
        return result;
    }

    static std::string to_std_string(CXString str)
    {
        std::string result;
        const char * s = clang_getCString(str);
        if (s != nullptr) result = s;
        clang_disposeString(str);
        return result;
    }

    struct completion_results
    {
        std::shared_ptr<CXCodeCompleteResults> results;
        typedef CXCompletionResult* iterator;

        completion_results(CXCodeCompleteResults* r)
        {
            this->results = std::shared_ptr<CXCodeCompleteResults>(r, &clang_disposeCodeCompleteResults);
        }

        iterator begin()
        {
            return results->Results;
        }

        iterator end()
        {
            return results->Results + results->NumResults;
        }
    };

    template<class F>
    static void for_each_completion_string(CXCompletionResult& c, F f)
    {
        if ( clang_getCompletionAvailability( c.CompletionString ) == CXAvailability_Available )
        {
            int num = clang_getNumCompletionChunks(c.CompletionString);
            for(int i=0;i<num;i++)
            {
                auto str = clang_getCompletionChunkText(c.CompletionString, i);
                auto kind = clang_getCompletionChunkKind(c.CompletionString, i);
                f(to_std_string(str), kind);
            }
        }
    }

    completion_results completions_at(unsigned line, unsigned col, const char * buffer, unsigned len)
    {
        if (buffer == nullptr) 
        {
            return clang_codeCompleteAt(this->tu, this->filename, line, col, nullptr, 0, CXCodeComplete_IncludeMacros);
        }
        else
        {
            auto unsaved = this->unsaved_buffer(buffer, len);
            return clang_codeCompleteAt(this->tu, this->filename, line, col, &unsaved, 1, CXCodeComplete_IncludeMacros);
        }
    }
public:
    translation_unit(const char * filename, const char ** args, int argv) : filename(filename)
    {
        this->index = clang_createIndex(1, 1);
        this->tu = clang_parseTranslationUnit(index, filename, args, argv, NULL, 0, clang_defaultEditingTranslationUnitOptions());
        // this->tu = clang_createTranslationUnitFromSourceFile(index, filename, argv, args, 0, NULL);
    }

    translation_unit(const translation_unit&) = delete;

    void reparse(const char * buffer=nullptr, unsigned len=0)
    {
        std::lock_guard<std::mutex> lock(this->m);
        if (buffer == nullptr) clang_reparseTranslationUnit(this->tu, 0, nullptr, 0);
        else
        {
            auto unsaved = this->unsaved_buffer(buffer, len);
             clang_reparseTranslationUnit(this->tu, 1, &unsaved, 0);
        }
    }

    std::set<std::string> complete_at(unsigned line, unsigned col, const char * prefix, const char * buffer=nullptr, unsigned len=0)
    {
        std::lock_guard<std::mutex> lock(this->m);
        std::set<std::string> results;
        for(auto& c:this->completions_at(line, col, buffer, len))
        {
            std::string r;
            for_each_completion_string(c, [&](const std::string& s, CXCompletionChunkKind kind)
            {
                if (kind == CXCompletionChunk_TypedText)
                {
                    r = s;
                }
            });
            if (!r.empty() and starts_with(r.c_str(), prefix)) results.insert(r);
        }
        return results;
    }

    std::vector<std::string> get_diagnostics()
    {
        std::lock_guard<std::mutex> lock(this->m);
        std::vector<std::string> result;
        auto n = clang_getNumDiagnostics(this->tu);
        for(int i=0;i<n;i++)
        {
            auto diag = std::shared_ptr<void>(clang_getDiagnostic(this->tu, i), &clang_disposeDiagnostic);
            if (diag != nullptr and clang_getDiagnosticSeverity(diag.get()) != CXDiagnostic_Ignored)
            {
                auto str = clang_formatDiagnostic(diag.get(), clang_defaultDiagnosticDisplayOptions());
                result.push_back(to_std_string(str));
            }
        }
        return result;
    }
    
    ~translation_unit()
    {
        clang_disposeTranslationUnit(this->tu);
        clang_disposeIndex(this->index);
    }
};

#ifndef CLANG_COMPLETE_ASYNC_WAIT_MS
#define CLANG_COMPLETE_ASYNC_WAIT_MS 200
#endif

class async_translation_unit : public translation_unit
{

    struct query
    {
        std::timed_mutex m;
        std::future<std::set<std::string>> results_future;
        std::set<std::string> results;
        unsigned line;
        unsigned col;

        query() : line(0), col(0)
        {}

        std::pair<unsigned, unsigned> get_loc()
        {
            // std::lock_guard<std::timed_mutex> lock(this->m);
            return std::make_pair(this->line, this->col);
        }

        void set(std::future<std::set<std::string>> && results_future, unsigned line, unsigned col)
        {
            this->results = {};
            this->results_future = std::move(results_future);
            this->line = line;
            this->col = col;
        }

        std::set<std::string> get(int timeout)
        {
            if (timeout > 0 and results_future.valid() and results_future.wait_for(std::chrono::milliseconds(timeout)) == std::future_status::ready)
            {
                this->results = this->results_future.get();
            }
            return this->results;
        }

    };

    query q;

public:
    async_translation_unit(const char * filename, const char ** args, int argv) : translation_unit(filename, args, argv)
    {}


    std::set<std::string> async_complete_at(unsigned line, unsigned col, const char * prefix, int timeout, const char * buffer=nullptr, unsigned len=0)
    {
        if (std::make_pair(line, col) != q.get_loc())
        {
            std::string buffer_as_string(buffer, buffer+len);
            this->q.set(detach_async([=]
            {
                // TODO: Should we always reparse?
                // this->reparse(buffer, len);
                auto b = buffer_as_string.c_str();
                if (buffer == nullptr) b = nullptr;
                return this->complete_at(line, col, "", b, buffer_as_string.length()); 
            }), line, col);
        }
        auto completions = q.get(timeout);
        std::set<std::string> results;
        std::copy_if(completions.begin(), completions.end(), inserter(results, results.begin()), [&](const std::string& x)
        { 
            return starts_with(x.c_str(), prefix); 
        });
        return results;
    }
};



#ifndef CLANG_COMPLETE_MAX_RESULTS
#define CLANG_COMPLETE_MAX_RESULTS 8192
#endif

struct translation_unit_data
{
    translation_unit_data(const char * filename, const char ** args, int argv) : tu(filename, args, argv)
    {}

    async_translation_unit tu;

    std::set<std::string> last_completions;
    const char * completions[CLANG_COMPLETE_MAX_RESULTS+2];

    std::vector<std::string> last_diagnostics;
    const char * diagnostics[CLANG_COMPLETE_MAX_RESULTS+2];
};

std::timed_mutex global_mutex;

std::unordered_map<std::string, std::shared_ptr<translation_unit_data>> tus;

std::shared_ptr<translation_unit_data> get_tud(const char * filename, const char ** args, int argv)
{
    if (tus.find(filename) == tus.end())
    {
        tus[filename] = std::make_shared<translation_unit_data>(filename, args, argv);
    }
    return tus[filename];
}

template<class Range, class Array>
void export_array(const Range& r, Array& out)
{
    auto overflow = r.size() > CLANG_COMPLETE_MAX_RESULTS;
    
    auto first = r.begin();
    auto last = overflow ? std::next(first, CLANG_COMPLETE_MAX_RESULTS) : r.end();
    std::transform(first, last, out, [](const std::string& x) { return x.c_str(); });

    out[std::distance(first, last)] = ""; 
}


extern "C" {
const char ** clang_complete_get_completions(
        const char * filename, 
        const char ** args, 
        int argv, 
        unsigned line, 
        unsigned col, 
        const char * prefix, 
        int timeout,
        const char * buffer, 
        unsigned len)
{
    static const char * empty_result[1] = { "" };
    std::unique_lock<std::timed_mutex> lock(global_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(10))) return empty_result;

    auto tud = get_tud(filename, args, argv);
    tud->last_completions = tud->tu.async_complete_at(line, col, prefix, timeout, buffer, len);
    
    export_array(tud->last_completions, tud->completions);

    return tud->completions;
}

const char ** clang_complete_get_diagnostics(const char * filename, const char ** args, int argv)
{
    static const char * empty_result[1] = { "" };
    std::unique_lock<std::timed_mutex> lock(global_mutex, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(250))) return empty_result;

    auto tud = get_tud(filename, args, argv);
    tud->tu.reparse(nullptr, 0);

    tud->last_diagnostics = tud->tu.get_diagnostics();
    
    export_array(tud->last_diagnostics, tud->diagnostics);

    return tud->diagnostics;
}

void clang_complete_reparse(const char * filename, const char ** args, int argv, const char * buffer, unsigned len)
{
    std::lock_guard<std::timed_mutex> lock(global_mutex);
    auto tud = get_tud(filename, args, argv);

    tud->tu.reparse(buffer, len);
}

void clang_complete_free_tu(const char * filename)
{
    std::lock_guard<std::timed_mutex> lock(global_mutex);
    if (tus.find(filename) != tus.end())
    {
        tus.erase(filename);
    }
}
}


#endif