/*
 ============== parallel copy ==============
compile:
g++ -O3 -std=c++17 -pthread my_copy.cpp -o my_copy

workflow :
- startup-thread list directory and distributes work a.k.a. directories to threads
- each thread create directories it find in destination
- copies files
- gets next directory from global directory string-vectors


*/

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <locale>
#include <chrono>
#include <filesystem>
#include <functional>  // std::ref
#include <iostream>
#include <fstream>
#include <thread>
#include <array>
#include <vector>
#include <unordered_map>

#include <mutex>
#include <exception>
#include <cstdlib>
#include <dirent.h> 
#include <sys/stat.h>
#include <chrono>
#include <typeinfo>
#include <atomic>
#include <ctime>
#include <new>

thread_local std::string tls_path;
thread_local std::vector<std::string> tls_filenames;
thread_local std::vector<std::string> tls_directories;

thread_local std::vector<std::string> tls_temp_vector;

//struct container{std::mutex mtx; std::vector<std::string> vec;};
//struct container{std::mutex mtx; std::unordered_multimap<std::string, std::vector<std::string> map> global_dirmap;} mycontainer;
std::string arg_source, arg_dest;
std::unordered_multimap<std::string, std::vector<std::string>> global_dirmap;
std::mutex global_dirmap_mutex;

std::vector<std::string> global_leftover_files_to_copy; // each thread copies ~20 files and puts the rest into this vector
std::mutex global_leftover_files_to_copy_mutex;

// global directory vectors & mutexes
// each thread puts directories into global vectors

#define NR_GLOBAL_FILENAME_MUTEXES 8 // also works surprisingly well with 1
std::array<std::mutex, NR_GLOBAL_FILENAME_MUTEXES> global_filename_mutexes; // mutexes for the following:
std::array<std::vector<std::string>, NR_GLOBAL_FILENAME_MUTEXES> filename_array_of_vectors;

#define NR_GLOBAL_DIRECTORIES_MUTEXES 8 // also works surprisingly well with 1
std::array<std::mutex, NR_GLOBAL_DIRECTORIES_MUTEXES> global_directories_mutexes; // mutexes for the following:
std::array<std::vector<std::string>, NR_GLOBAL_DIRECTORIES_MUTEXES> directories_array_of_vectors;

// global vectors for initial startup: filenames & directories
std::vector<std::string> global_filenames(0);
std::vector<std::string> global_directories(0);

// vector to save threads' exceptions
std::vector<std::exception_ptr> global_exceptions;
std::mutex coutmtx, global_exceptmutex; // mutex for std::cout and for exceptions thrown in threads

// atomics to indicate busy threads, limited atomics to limit checks in loop
#define NR_ATOMICS 8
std::array<std::atomic<int>, NR_ATOMICS> atomic_running_threads;

// vector to save statistics: optimal if threads running time is evenly distributed
std::vector<uint64_t> running_times;
std::mutex running_times_mutex;
std::vector<uint64_t> starting_times;
std::mutex starting_times_mutex;

uint64_t hits{0};   
std::string exe_name;

class Worker {
   public:
    Worker(int n, std::string s) : worker_id(n), start_with_path(s) {}
    
    void operator()() {
        try {
            if (start_with_path != "") {

                tls_path = start_with_path;
                do_linear_descent();
                std::this_thread::sleep_for(std::chrono::milliseconds(0 + worker_id % 8));
                working();

            } else {

                std::this_thread::sleep_for(std::chrono::milliseconds(10 + worker_id % 8)); // heuristic to wait for other threads to find some dirs
                working();
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(global_exceptmutex);
            global_exceptions.push_back(std::current_exception());
        }
    }

    void working() {
#ifdef DIE_DEBUG
        auto starting_time = std::chrono::high_resolution_clock::now();
        uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(starting_time.time_since_epoch()).count();
#endif


#define RETRY_COUNT 1 
        int retry_count = RETRY_COUNT;
        int dont_wait_forever = 640;  // to eliminate endless waiting at the 'end of the tree':few dirs left

    check_again:
        int proceed_new = 0;
#define NR_OF_ROUNDS 1  // a few or just one round works equally well
        for (int i = 0; proceed_new == 0 && i < NR_OF_ROUNDS * NR_GLOBAL_DIRECTORIES_MUTEXES; i++) {
            int start_with_i = (worker_id + i) % NR_GLOBAL_DIRECTORIES_MUTEXES;  // start with own id-vector
            std::lock_guard<std::mutex> lock(global_directories_mutexes[start_with_i]);
            if (!directories_array_of_vectors[start_with_i].empty()) {
                tls_path = directories_array_of_vectors[start_with_i].back();
                directories_array_of_vectors[start_with_i].pop_back();
                proceed_new = 1;
                break;
            }
        }
        if (proceed_new == 1) {         // equal to if(tls_path != "")
            retry_count = RETRY_COUNT;  // reset retry_count
            do_linear_descent();        // uses tls_path for next directory to step into

        } else {
            goto end_label;             // thread will die here eventually
        }

        goto check_again;               // after returning from do_linear_descent() in if(proceed_new==1) above, check if there is more work

    end_label:
        if (retry_count-- > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(510));
            goto check_again;
        }

        else {
            
    copy_till_done:
                {  // start of lock-scope
                    std::lock_guard<std::mutex> lock(global_leftover_files_to_copy_mutex);
                    if (global_leftover_files_to_copy.size() > 0) {
#define N_TO_COPY 12
                        // heuristic: copy a bunch if there are enough than one by one so more threads get work
                        if (global_leftover_files_to_copy.size() >= N_TO_COPY) {
                            for (int i = 0; i < N_TO_COPY; i++) {
                                tls_temp_vector.emplace_back(global_leftover_files_to_copy.back());
                                global_leftover_files_to_copy.pop_back();
                            }
                        } else {
                            tls_temp_vector.emplace_back(global_leftover_files_to_copy.back());
                            global_leftover_files_to_copy.pop_back();
                        }
                    }
                }  // end of lock-scope

                do_file_copy_from_string_vector(tls_temp_vector);

                tls_temp_vector.clear();

                if (global_leftover_files_to_copy.size() > 0) {
                    goto copy_till_done;
                }

                for (int i = 0; i < NR_ATOMICS; i++) {
                    if (atomic_running_threads[i] > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(717));
                        
                        goto copy_till_done;
                    }
                }

#ifdef DIE_DEBUG
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::nanoseconds ns = end_time - starting_time;
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(ns).count();
            {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::cout << "worker: " << worker_id << " FINALLY DIED after: " << elapsed << " microseconds " << elapsed / 1000 << " millisecs " << elapsed / 1000000 << " secs" << std::endl;
            }

            {
                std::lock_guard<std::mutex> lock(running_times_mutex);
                running_times.push_back(static_cast<uint64_t>(elapsed));
            }
#endif

            return;
        }
    }

    void do_file_copy_from_string_vector(std::vector<std::string> &svector) {
        atomic_running_threads[worker_id % NR_ATOMICS]++;  // while copying block other threads from quitting
        auto len = arg_source.length();
        for (auto &copyfrom : svector) {
            std::string copyto = arg_dest + copyfrom.substr(len);
            //const auto copyOptions = std::filesystem::copy_options::update_existing;
            const auto copyOptions = std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::copy_symlinks ;
            try {
                std::filesystem::copy_file(copyfrom, copyto, copyOptions);

                std::filesystem::path pfrom {copyfrom};
                std::filesystem::path pto {copyto};

                //std::cout << "copy from " << copyfrom << " copyto " << copyto << std::endl;
                auto ftime = std::filesystem::last_write_time(pfrom);
                std::filesystem::last_write_time(pto, ftime);
 
            } catch (std::filesystem::filesystem_error &e) {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::cout << "Could not copy:" << copyfrom << "->" << copyto << e.what() << '\n';
            }
        }
        atomic_running_threads[worker_id % NR_ATOMICS]--;
    }

    /*
    descent into ONE subdirectory found by `do_tree_walking()` linearly,
    meaning: step into ONE directory after another, till the end of this branch of the directory tree
        e.g. /A/A/A with each directory having dirs A-Z
        add the rest B-Z to global vectors for other threads to pick up
    */

    void do_linear_descent() {  // list dir, step into first dir till end

        // increase atomic counter, we might find new directories in this code section: check for running threads before quitting
        atomic_running_threads[worker_id % NR_ATOMICS]++;

        do {
#ifdef DEBUG
            if (tls_path == "") {
                std::lock_guard<std::mutex> lock(coutmtx);
                std::cout << worker_id << ": ERROR tls_path " << tls_path << std::endl;
            }
#endif

            do_dir_walking(tls_path); // also splits files to tls_filenames (COPYMAX:4) and global_leftover_files_to_copy 
            do_file_copy_from_string_vector(tls_filenames); // the COPYMAX:4 files from do_dir_walking
            tls_filenames.clear();
            

            // get one directory
            if (!tls_directories.empty()) {
                tls_path = tls_directories.back();

                tls_directories.pop_back();
            } else {
                tls_path = "";  // don't walk into same dir again
            }

            // save the residual dirs
            {
                std::lock_guard<std::mutex> lock(global_directories_mutexes[worker_id % NR_GLOBAL_DIRECTORIES_MUTEXES]);
                for (auto const &v : tls_directories) {
                    directories_array_of_vectors[worker_id % NR_GLOBAL_DIRECTORIES_MUTEXES].emplace_back(v);
                }
            }
            tls_directories.clear();

        } while (tls_path != "");

        // decrease atomic counter
        atomic_running_threads[worker_id % NR_ATOMICS]--;
    }

   private:
    const int worker_id = -171717;
    std::string start_with_path;
    // smaller means earlier exit on binary files
    
    int do_dir_walking(const std::string &sInputPath) {

        int error_code = 1;

        // processing each file in directory

        DIR *dir_handle;
        struct dirent *dir;
        dir_handle = opendir(sInputPath.c_str());
        if (dir_handle) {
            error_code = 0;
          
            while ((dir = readdir(dir_handle)) != NULL) {

                if (dir->d_type == DT_REG) {  // found regular file
                    
                    std::string sFileName = sInputPath + dir->d_name;
                    tls_filenames.emplace_back(sFileName);

                } else if (dir->d_type == DT_LNK) {

                    std::string linkname = sInputPath + dir->d_name;
                    auto len = arg_source.length();
                    std::string create_target = arg_dest + linkname.substr(len);
                    std::filesystem::copy_symlink(linkname, create_target);


                } else if (dir->d_type == DT_DIR) {  // found directory

                    std::string sname = dir->d_name;

                    if (sname != "." && sname != "..") {

                        tls_directories.emplace_back(sInputPath + sname + "/");
                        std::string dir_string = sInputPath + sname;
                        auto len = arg_source.length();  // delete srcdirectory string
                        std::string create_target = arg_dest + dir_string.substr(len);                        
                        std::filesystem::path dir_to_create{create_target};
                        std::filesystem::create_directory(dir_to_create, dir_string); // create_dir(new, take-attributes-from-old)
                        
                    }
                }
            }
            closedir(dir_handle);

            do_split_TLS_FILENAMES_between_local_and_global(); // give other threads some files to copy?

            return error_code;
        } else {
            std::lock_guard<std::mutex> lock(coutmtx);
            std::cerr << "Cannot open input directory: " << sInputPath << std::endl;
            return error_code;
        }
    }

   public:

    void do_split_TLS_FILENAMES_between_local_and_global() {
        // tls_filenames to globalleftover
#define COPYMAX  4
        if (tls_filenames.size() >= COPYMAX) {
            std::lock_guard<std::mutex> lock(global_leftover_files_to_copy_mutex);
            //global_leftover_files_to_copy.insert(global_leftover_files_to_copy.end(), tls_filenames.begin(), tls_filenames.begin() + COPYMAX);
            for (int i = COPYMAX; i < tls_filenames.size(); i++) {
                global_leftover_files_to_copy.emplace_back(tls_filenames.back());
                tls_filenames.pop_back();
            }
        }
    }

};  // end of class Worker

void do_startup_file_walking(std::string starting_path) {
    try {
        
        std::filesystem::path path(starting_path);
        path = std::filesystem::canonical(path);
        std::string canonical_starting_path = std::filesystem::canonical(path);
        std::filesystem::directory_iterator dir_iter(path, std::filesystem::directory_options::skip_permission_denied);
        std::filesystem::directory_iterator end;
        while (dir_iter != end) {
            std::filesystem::path path = dir_iter->path();
            if ((std::filesystem::is_directory(path) || std::filesystem::is_regular_file(path)) || std::filesystem::is_symlink(path)) {
                
                std::string path_entry = path.string();
                if (std::filesystem::is_directory(path) && !std::filesystem::is_symlink(path)) {

                    if (path_entry != "/proc" && path_entry != "/sys") {
                        global_directories.emplace_back(path_entry + "/");  // add trailing /
                    }

                    auto len = canonical_starting_path.length();        // delete srcdirectory string
                    std::string create_target = arg_dest + path_entry.substr(len); // e.g. /A or /B or /C
                    
                    std::filesystem::path dir_to_create{create_target};
                    
                    std::filesystem::create_directory(dir_to_create, path_entry);

                } else if (std::filesystem::is_symlink(path)) {

                    std::string linkname = path;
                    auto len = arg_source.length();
                    std::string create_target = arg_dest + linkname.substr(len);
                    std::filesystem::copy_symlink(linkname, create_target);

                    
                    
                } else if (std::filesystem::is_regular_file(path)) {

                    global_leftover_files_to_copy.emplace_back(path_entry);
                }
            }
            ++dir_iter;
        }
    } catch (const std::exception &e) {
        std::cerr << "exception: do_startup_file_walking(): " << e.what() << '\n';
    }
}

int main(int argc, char* argv[]) {

    exe_name = argv[0];
    
    int n_threads = -1;
    n_threads = std::thread::hardware_concurrency();
    if (argc > 1) {
                try {
                n_threads=std::stoi(argv[1]);}
                catch (const std::exception &e){
                    std::cerr << "argv1 is no integer in function: " << e.what() << std::endl;
                    return -1;
                }
    }
    
    if(argc == 4){
        arg_source = std::string(argv[2]);
        arg_dest   = std::string(argv[3]);
        std::filesystem::path srcpath(arg_source);
        arg_source = std::filesystem::canonical(srcpath);
        std::filesystem::path dstpath(arg_dest);
        std::filesystem::create_directory(dstpath);
        arg_dest = std::filesystem::canonical(dstpath);

    } else {
        std::cout << "error with argv[2] usage: " << argv[0] << " <number of threads> <source path> <dest path>"  << std::endl; 
        return -1;
    }
    
    std::cout << "Copy from: " << arg_source << " to " << arg_dest << std::endl; 
    
    std::vector<std::thread> threads;
    std::vector<Worker> workers;
    global_exceptions.clear();

    if(arg_source != ""){
        do_startup_file_walking(arg_source);
    } else {
        std::cout << "error with arg_source" << std::endl;
    }

    int starting_dirs = global_directories.size();

    for(int i=0; i < n_threads; ++i){
        if(starting_dirs < n_threads){
            if(i < starting_dirs){
                workers.push_back(Worker(i, global_directories.back())); // same
                global_directories.pop_back();                           // same
            } else {
                workers.push_back( Worker(i, "") );
            }
        } else {
            workers.push_back(Worker(i, global_directories.back()));     // same
            global_directories.pop_back();                               // same, re-arrange?
        }   
    }

    // fill queues
    int tmp = 0;
    while(!global_directories.empty()) {
        directories_array_of_vectors[(tmp++%n_threads)%NR_GLOBAL_DIRECTORIES_MUTEXES].push_back(global_directories.back());
        global_directories.pop_back();
    }

    // start threads(Worker)
    for(int i=0; i < n_threads; ++i){
        threads.emplace_back( std::ref(workers[i]) );
    }

    for(auto &v: threads){
        v.join();
    }

    /* process exceptions from threads */
    for(auto const &e: global_exceptions){
        try{
            if(e != nullptr){
                std::rethrow_exception(e);
            }
        } 
        catch(std::exception const &ex) {
                std::cerr << "EZZOZ exception: " << ex.what() << std::endl;
        }
    }

    if(global_directories.empty()){
    #ifdef DEBUG        
        std::cout << "global_directories is empty" << std::endl;
    #endif
    }
    else {
        std::cout << "ERROR: global_directories HAS MORE WORK => needs next round of threads!" << std::endl;
        for(auto const& v: global_directories){
            std::cout << "globdirs-residuals: " << v << std::endl;
        }
    }   

    for(int i=0; i<NR_GLOBAL_DIRECTORIES_MUTEXES; i++){
        if(!directories_array_of_vectors[i].empty()){
            std::cout << "MORE WORK: directories_array_of_vectors[" << i << "].size(): " << directories_array_of_vectors[i].size() << std::endl;
            for(auto const &v: directories_array_of_vectors[i]){
                std::cout << "directories_array_of_vectors[" << i << "] = " << v << std::endl;
            }
        }
    }

#ifdef DIE_DEBUG // print threads' elapsed runtime:
    std::sort(running_times.begin(), running_times.end());
    std::cout << "\"running\" times" << std::endl;
    for(auto v: running_times){
        std::cout << v << std::endl;
    }
    std::cout << "MAX_MIN:" << std::endl;
    auto minmax = std::minmax_element(running_times.begin(), running_times.end());
    // std::make_pair(first, first)
    if(minmax != std::make_pair(running_times.begin(),running_times.begin() ) ){
        std::cout << "max: " << *minmax.second << " min: " << *minmax.first << std::endl;
    }
    auto dif = *minmax.second - *minmax.first;
    std::cout << "difference(microsecs) between max and min: " << dif << " millisecs: " << dif/1000 << std::endl;
#endif


} // end int main(int argc, char* argv[])

