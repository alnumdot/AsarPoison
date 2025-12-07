#include <windows.h>
#include <iostream>
#include "json.hpp"

//todo indicator flag ob asar schonmal angefasst wurde oder nicht
//todo nach update wieder resolve_path zum checken (gute idee zum checken eif)

using json = nlohmann::json;

int round_up(int i, int m) {
    return (i + m - 1) & ~(m - 1);
}

#pragma pack(push, 1)
struct AsarStart {           // (int)32bit * 4 = 16 byte
    int data_size;           // 4
    int header_size;         // header size + padding + 8
    int header_object_size;  // header size + padding + data_size (4)
    int header_string_size;  // ganze header size

    AsarStart() : data_size(4), header_size(0), header_object_size(0), header_string_size(0) {}

    explicit AsarStart(int header_sz) : data_size(4), header_string_size(header_sz) {
        auto aligned_size = round_up(header_string_size, 4);
        header_size = aligned_size + 8;
        header_object_size = aligned_size + data_size;
    }
};
#pragma pack(pop)

struct AsarFileOffset {
    int offset;
    int size;

    AsarFileOffset() : offset(0), size(0) {}

    explicit AsarFileOffset(const std::string& offs, int sz) : offset(std::stoi(offs)), size(sz) {}
};

class AsarFile {
private:
    HANDLE file = 0;                 //windows file handle
    HANDLE mapping = 0;              //file mapping handle
    char* mapped_region = nullptr;         //pointer to mapped file memory
    LARGE_INTEGER file_size;     //size of the ASAR file

    std::string path;            //path to ASAR file
    AsarStart* asar_start = nullptr;       //pointer to ASAR header
    int data_start;              //start of file data section

    // header metadata
    AsarFileOffset header;       //header location/size
    std::string header_raw;      //raw header JSON string
    json header_json;            //parsed header JSON

    // package.json metadata
    AsarFileOffset package;      //package.json location/size
    json package_json;           //parsed package.json

    // main script metadata
    AsarFileOffset mainjs;       //main JS file location/size
    bool is_ES_module;           //flag for ES module type


public:
    AsarFile(std::string& filepath) : data_start(0), path(filepath), is_ES_module(false) {}

    bool init() {
        std::cout << "opening file: " << path << "\n";

        file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) std::cerr << "ERROR: file not found\n";
            if (error == ERROR_ACCESS_DENIED) std::cerr << "ERROR: access denied\n";
            if (error == ERROR_SHARING_VIOLATION) std::cerr << "ERROR: file is in use by another process\n";
            if (error == ERROR_PATH_NOT_FOUND) std::cerr << "ERROR: path not found\n";

            return false;
        }

        if (!GetFileSizeEx(file, &file_size)) {
            CloseHandle(file);
            return false;
        }

        mapping = CreateFileMappingA(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!mapping) {
            CloseHandle(file);
            return false;
        }

        mapped_region = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!mapped_region) {
            CloseHandle(mapping);
            CloseHandle(file);
            return false;
        }

        //parse ASAR header
        asar_start = (AsarStart*)mapped_region;

        data_start = round_up(16 + asar_start->header_string_size, 4);

        //rxtract header JSON
        header.offset = 16 - data_start;
        header.size = asar_start->header_string_size;

        try {
            header_raw = read_data<std::string>(header);

            header_json = json::parse(header_raw);
        }
        catch (const std::exception& e) {
            UnmapViewOfFile(mapped_region);
            CloseHandle(mapping);
            CloseHandle(file);
            return false;
        }

        //rxtract package.json
        try {
            if (!header_json.contains("files") || !header_json["files"].contains("package.json")) {
                return false;
            }

            package = AsarFileOffset(
                header_json["files"]["package.json"]["offset"],
                header_json["files"]["package.json"]["size"]
            );
            package_json = read_data<json>(package);
        }
        catch (const std::exception& e) {
            UnmapViewOfFile(mapped_region);
            CloseHandle(mapping);
            CloseHandle(file);
            return false;
        }

        // Check if ES Module
        if (package_json.contains("type")) {
            if (package_json["type"] == "module")
                is_ES_module = true;
        }

        // Resolve main script path
        try {
            if (!package_json.contains("main")) {
                return false;
            }

            std::string main_path = package_json["main"];

            mainjs = resolve_path(main_path);
        }
        catch (const std::exception& e) {
            UnmapViewOfFile(mapped_region);
            CloseHandle(mapping);
            CloseHandle(file);
            return false;
        }

        return true;
    }

    bool repack(std::string& inj_line_COMMON, std::string& inj_line_ESM) {
        std::string& inj_line = is_ES_module ? inj_line_ESM : inj_line_COMMON;
        auto inj_len = static_cast<long long>(inj_line.length());


        update_header(header_raw, mainjs.offset, inj_len);

        AsarStart new_asar_start = AsarStart(header_raw.length());
        int aligned_size = round_up(new_asar_start.header_string_size, 4);
        int diff = aligned_size - new_asar_start.header_string_size;
        long long new_size = 16 + aligned_size + ((long long)file_size.QuadPart - data_start) + inj_len;


        std::vector<char> buffer(new_size);
        int cur_pos = 0;

        //write ASAR start
        memcpy(buffer.data(), &new_asar_start, sizeof(AsarStart));
        cur_pos = 16;

        //write header
        memcpy(buffer.data() + cur_pos, header_raw.data(), header_raw.length());
        cur_pos += aligned_size;

        //copy data BEFORE injection point
        long long before_injection_size = mainjs.offset;
        memcpy(
            buffer.data() + cur_pos,
            mapped_region + data_start,
            before_injection_size
        );
        cur_pos += before_injection_size;

        memcpy(buffer.data() + cur_pos, inj_line.data(), inj_len);
        cur_pos += inj_len;

        //copy original main.js and rest of data
        long long remaining_size = file_size.QuadPart - (data_start + mainjs.offset);
        memcpy(
            buffer.data() + cur_pos,
            mapped_region + data_start + mainjs.offset,
            remaining_size
        );
        cur_pos += remaining_size;

        //unmap before writing
        if (!UnmapViewOfFile(mapped_region)) {
            return false;
        }
        mapped_region = nullptr;

        if (mapping) {
            if (!CloseHandle(mapping)) {
                return false;
            }
            mapping = nullptr;
        }

        //reset file pointer to beginning
        LARGE_INTEGER li_begin = { 0 };
        if (!SetFilePointerEx(file, li_begin, nullptr, FILE_BEGIN)) {
            return false;
        }

        //write the buffer
        dump_file(buffer);

        //set new file size
        LARGE_INTEGER li;
        li.QuadPart = new_size;
        if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN)) {
            return false;
        }

        if (!SetEndOfFile(file)) {
            return false;
        }

        return true;
    }

private:

    void dump_file(const std::vector<char>& buffer) { //MAXDWORD == max chunk size
        auto size_left = buffer.size();
        DWORD written_bytes = 0;
        const char* current_position = buffer.data();

        while (size_left > 0) {
            DWORD chunk_size = (size_left > MAXDWORD) ? MAXDWORD : static_cast<DWORD>(size_left);

            if (!WriteFile(file, current_position, chunk_size, &written_bytes, nullptr))
                return;

            size_left -= written_bytes;
            current_position += written_bytes;
        }
    }

    template<typename T>
    T read_data(AsarFileOffset file_info) { //struct als argument bissl chilliger 
        char* data_ptr = mapped_region + data_start + file_info.offset;
        auto buffer = std::string(data_ptr, file_info.size);

        if constexpr (std::is_same_v<T, json>) {
            return json::parse(buffer);
        }
        return buffer;
    }

    //sucht size im offset json nest
    int find_connected_size(std::string& header, int main_offset_index, int& ex_size_value, int& size_value_str_len) { //ghetto
        bool forward = true;
    find:
        size_t size_index;
        if (forward)
            size_index = header.find("\"size\":", main_offset_index) + 7;
        else
            size_index = header.rfind("\"size\":", main_offset_index) + 7;

        int num = 0;
        do {
            num++;
        } while (isdigit(header[size_index + num]));

        size_t size_end_index = size_index + num;

        std::string size_value_str = header.substr(size_index, size_end_index - size_index);
        int size_value = std::stoi(size_value_str);

        //dreht dann von find zu rfind um weil size wahrscheinlich vor offset und nicht danach steht
        if (size_value != mainjs.size) {
            if (!forward)
                return 0;

            forward = !forward;
            goto find;
        }

        ex_size_value = size_value;
        size_value_str_len = size_value_str.length();

        return size_index;
    }

    void update_header(std::string& header, int target_offset, int inj_len) { //ghetto
        int cur_index = 0;

        while (true) {
            cur_index = header.find("\"offset\":", cur_index + 1);

            if (cur_index == std::string::npos)
                break;

            cur_index += 10;
            size_t cur_end_index = header.find('"', cur_index);

            std::string cur_offset_str = header.substr(cur_index, cur_end_index - cur_index);
            int cur_offset = std::stoi(cur_offset_str);

            if (cur_offset > target_offset) {
                std::string new_str = std::to_string(cur_offset + inj_len);
                header.replace(cur_index, cur_offset_str.length(), std::to_string(cur_offset + inj_len));
            }

            else if (cur_offset == target_offset) {
                int size_value;
                int size_value_str_len;
                size_t size_index = find_connected_size(header, cur_index, size_value, size_value_str_len);
                header.replace(size_index, size_value_str_len, std::to_string(size_value + inj_len));
            }
        };
    }

    // path von package.json[main] sieht so aus ca. lib/main.js //ghetto
    std::vector<std::string> json_path_to_list(std::string& json_path) {
        std::vector<std::string> path;
        std::string item;
        for (char& a : json_path) {
            if (a == '\\' || a == '/') {
                path.push_back(item);
                item.clear();
                continue;
            }
            item += a;
        }
        path.push_back(item);
        return path;
    }

    //looped durch die path childs um halt dann den letzten dings zu finden //ghetto
    AsarFileOffset resolve_path(std::string& path) {
        auto path_parts = json_path_to_list(path);
        json destination = header_json["files"];

        for (int i = 0; i < path_parts.size() - 1; i++) {
            destination = destination[path_parts[i]]["files"];
        }

        std::string name = path_parts[path_parts.size() - 1];
        destination = destination[name];
        return AsarFileOffset(destination["offset"], destination["size"]);
    }

};

int main() {
    std::string fp = "C:\\...\\...\\...\\...\\...\\...\\...\\app.asar";
    AsarFile af = AsarFile(fp);

    if (!af.init()) printf("something went wrong in af.init()\n");

    //different lines for different JS kinds
    std::string inj_str_CJS = "require('child_process').exec('msg * \"injected!!\"');\n"; //Common JS
    std::string inj_str_ESM = "import { exec } from 'child_process';exec('msg * \"injected!\"');\n"; //ES Module

    if (!af.repack(inj_str_CJS, inj_str_ESM)) printf("something went wrong in af.repack()\n");
}