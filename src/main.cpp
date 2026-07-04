#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------
// 1. LOW-LEVEL BYTE UTILITIES
// ---------------------------------------------------------

uint16_t decode_size(uint64_t serial_type) {
  if (serial_type >= 13 && serial_type % 2 == 1)
    return (serial_type - 13) / 2; // TEXT
  if (serial_type >= 12 && serial_type % 2 == 0)
    return (serial_type - 12) / 2; // BLOB
  if (serial_type == 0 || serial_type == 8 || serial_type == 9)
    return 0; // NULL, 0, 1
  if (serial_type == 1)
    return 1; // 8-bit int
  if (serial_type == 2 || serial_type == 3 || serial_type == 4)
    return serial_type; // 16, 24, 32-bit ints
  if (serial_type == 5)
    return 6; // 48-bit int
  if (serial_type == 6 || serial_type == 7)
    return 8; // 64-bit int / float
  return 0;
}

class DatabaseUtils {
private:
  std::ifstream &db;

public:
  DatabaseUtils(std::ifstream &database_file) : db(database_file) {}

  void seekg(uint64_t pos) { db.seekg(pos); }
  uint64_t tellg() { return db.tellg(); }
  void read(char *buffer, size_t size) { db.read(buffer, size); }

  uint64_t read_var() {
    uint64_t readval = 0;
    uint8_t ip;
    for (int i = 0; i < 9; i++) {
      db.read(reinterpret_cast<char *>(&ip), 1);
      if (i == 8) {
        readval = (readval << 8) | ip;
        break;
      }
      bool has_next = (ip >> 7) == 1;
      ip &= 0x7F;
      readval = (readval << 7) | ip;
      if (!has_next)
        break;
    }
    return readval;
  }

  uint32_t read_integer(int bytes) {
    uint32_t result = 0;
    for (int i = 0; i < bytes; i++) {
      uint8_t byte;
      db.read(reinterpret_cast<char *>(&byte), 1);
      result = (result << 8) | byte;
    }
    return result;
  }
};

// ---------------------------------------------------------
// 2. TABLE CLASS
// ---------------------------------------------------------

class Table {
private:
  DatabaseUtils &db;
  uint32_t root_page;
  uint16_t page_size;
  std::string sql;

  int get_column_index(const std::string &target_col) {
    size_t start = sql.find('(');
    size_t end = sql.find_last_of(')');
    if (start == std::string::npos || end == std::string::npos)
      return -1;

    std::string columns_str = sql.substr(start + 1, end - start - 1);
    std::stringstream ss(columns_str);
    std::string col_def;
    int index = 0;

    while (std::getline(ss, col_def, ',')) {
      size_t first_char = col_def.find_first_not_of(' ');
      if (first_char != std::string::npos)
        col_def = col_def.substr(first_char);
      if (col_def.find(target_col + " ") == 0 || col_def == target_col)
        return index;
      index++;
    }
    return -1;
  }

  uint64_t count_rows_recursive(uint32_t page_num) {
    uint64_t offset = (page_num - 1) * page_size;
    db.seekg(offset);

    uint8_t page_type;
    db.read(reinterpret_cast<char *>(&page_type), 1);

    db.seekg(offset + 3);
    uint16_t num_cells = db.read_integer(2);

    if (page_type == 0x0D)
      return num_cells;

    uint64_t total_rows = 0;
    if (page_type == 0x05) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 12 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);
        uint32_t left_child_page = db.read_integer(4);
        total_rows += count_rows_recursive(left_child_page);
      }
      db.seekg(offset + 8);
      uint32_t rightmost_page = db.read_integer(4);
      total_rows += count_rows_recursive(rightmost_page);
    }
    return total_rows;
  }

  void print_column_recursive(uint32_t page_num, int col_index) {
    uint64_t offset = (page_num - 1) * page_size;
    db.seekg(offset);

    uint8_t page_type;
    db.read(reinterpret_cast<char *>(&page_type), 1);

    db.seekg(offset + 3);
    uint16_t num_cells = db.read_integer(2);

    if (page_type == 0x0D) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 8 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);

        db.seekg(offset + cell_ptr);
        db.read_var(); // Payload Size
        db.read_var(); // RowID

        uint64_t header_start = db.tellg();
        uint64_t size_of_header = db.read_var();
        uint64_t bytes_read = db.tellg() - header_start;

        std::vector<uint64_t> serial_types;
        while (bytes_read < size_of_header) {
          uint64_t before = db.tellg();
          serial_types.push_back(db.read_var());
          bytes_read += (db.tellg() - before);
        }

        uint64_t data_offset = 0;
        for (int c = 0; c < col_index && c < serial_types.size(); c++) {
          data_offset += decode_size(serial_types[c]);
        }

        db.seekg(header_start + size_of_header + data_offset);

        if (col_index < serial_types.size()) {
          uint64_t target_serial = serial_types[col_index];
          uint16_t data_size = decode_size(target_serial);

          if (target_serial >= 13 && target_serial % 2 == 1) { // TEXT
            std::string column_data(data_size, '\0');
            db.read(&column_data[0], data_size);
            retrieval.push_back(column_data);
          } else {
            // Push an empty string so the indices don't get misaligned on NULLs
            retrieval.push_back("");
          }
        }
      }
      return;
    }

    if (page_type == 0x05) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 12 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);
        uint32_t left_child = db.read_integer(4);
        print_column_recursive(left_child, col_index);
      }
      db.seekg(offset + 8);
      uint32_t rightmost = db.read_integer(4);
      print_column_recursive(rightmost, col_index);
    }
  }

public:
  Table(DatabaseUtils &db_ref, uint32_t root, uint16_t size, std::string schema)
      : db(db_ref), root_page(root), page_size(size), sql(schema) {}

  bool is_valid() const { return root_page != 0; }

  uint64_t count_rows() { return count_rows_recursive(root_page); }

  std::vector<std::string> retrieval;

  void print_column(const std::string &col_name) {
    int col_idx = get_column_index(col_name);
    if (col_idx != -1) {
      print_column_recursive(root_page, col_idx);
    } else {
      std::cerr << "Column not found in schema!" << std::endl;
    }
  }
};

// ---------------------------------------------------------
// 3. DATABASE CLASS
// ---------------------------------------------------------

struct SchemaEntry {
  std::string type;
  std::string name;
  std::string tbl_name;
  uint32_t root_page;
  std::string sql;
};

class Database {
private:
  DatabaseUtils db;
  uint16_t page_size;

  std::vector<SchemaEntry> parse_schema() {
    std::vector<SchemaEntry> entries;
    db.seekg(103);
    uint16_t num_cells = db.read_integer(2);

    for (unsigned int pt = 0; pt < num_cells; pt++) {
      db.seekg(108 + pt * 2);
      uint16_t celloff = db.read_integer(2);

      db.seekg(celloff);
      db.read_var(); // Payload Size
      db.read_var(); // RowID

      uint64_t header_start = db.tellg();
      uint64_t size_of_header = db.read_var();

      uint64_t serial_type = db.read_var();
      uint64_t serial_name = db.read_var();
      uint64_t serial_tbl_name = db.read_var();
      uint64_t serial_rootpage = db.read_var();
      uint64_t serial_sql = db.read_var();

      uint64_t type_size = decode_size(serial_type);
      uint64_t name_size = decode_size(serial_name);
      uint64_t tbl_name_size = decode_size(serial_tbl_name);
      uint64_t rootpage_size = decode_size(serial_rootpage);
      uint64_t sql_size = decode_size(serial_sql);

      uint64_t data_start = header_start + size_of_header;
      SchemaEntry entry;

      db.seekg(data_start);
      entry.type = std::string(type_size, '\0');
      db.read(&entry.type[0], type_size);

      db.seekg(data_start + type_size);
      entry.name = std::string(name_size, '\0');
      db.read(&entry.name[0], name_size);

      db.seekg(data_start + type_size + name_size + tbl_name_size);
      entry.root_page = db.read_integer(rootpage_size);

      db.seekg(data_start + type_size + name_size + tbl_name_size +
               rootpage_size);
      entry.sql = std::string(sql_size, '\0');
      db.read(&entry.sql[0], sql_size);

      entries.push_back(entry);
    }
    return entries;
  }

public:
  Database(std::ifstream &file) : db(file) {
    db.seekg(16);
    page_size = db.read_integer(2);
  }

  void print_info() {
    std::cout << "database page size: " << page_size << std::endl;
    db.seekg(103);
    std::cout << "number of tables: " << db.read_integer(2) << std::endl;
  }

  void print_tables() {
    for (const auto &entry : parse_schema()) {
      if (entry.type == "table" && entry.name != "sqlite_sequence") {
        std::cout << entry.name << " ";
      }
    }
    std::cout << std::endl;
  }

  Table get_table(const std::string &target_table) {
    for (const auto &entry : parse_schema()) {
      if (entry.type == "table" && entry.name == target_table) {
        return Table(db, entry.root_page, page_size, entry.sql);
      }
    }
    return Table(db, 0, page_size, "");
  }
};

// ---------------------------------------------------------
// 4. MAIN (The Text Router)
// ---------------------------------------------------------

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  if (argc != 3)
    return 1;

  std::ifstream database_file(argv[1], std::ios::binary);
  if (!database_file)
    return 1;

  Database db(database_file);

  std::string command = argv[2];
  std::string lower_command = command;
  std::transform(lower_command.begin(), lower_command.end(),
                 lower_command.begin(), ::tolower);

  if (command == ".dbinfo") {
    db.print_info();
  } else if (command == ".tables") {
    db.print_tables();
  } else if (lower_command.find("select count(*)") != std::string::npos) {
    std::string table_name = command.substr(command.find_last_of(' ') + 1);
    Table table = db.get_table(table_name);

    if (table.is_valid())
      std::cout << table.count_rows() << std::endl;
    else
      std::cerr << "Table not found!" << std::endl;
  } else if (lower_command.find("select ") == 0) {
    std::vector<std::string> words;
    std::string cur;
    std::stringstream ss(command);

    // Safety against double spaces
    while (std::getline(ss, cur, ' ')) {
      if (!cur.empty())
        words.push_back(cur);
    }
    for (int i = 0; i < words.size(); i++) {
      if (words[i][0] == char(39)) {
        for (int j = i + 1; j < words.size(); j++)
          words[i] += " " + words[j];
        words.erase(words.begin() + i + 1, words.end());
      }
    }
    // std::cout << words.back() << "\n";
    if (words[words.size() - 4] != "WHERE") {
      // The table name is always the last word
      std::string table_name = words.back();

      // FIX 1: Fetch the table ONCE, outside the loop!
      Table table = db.get_table(table_name);

      if (!table.is_valid()) {
        std::cerr << "Table not found!" << std::endl;
        return 0;
      }

      std::vector<std::vector<std::string>> info;

      // Loop over the columns (words[1] to words[size-3])
      for (int i = 1; i <= words.size() - 3; i++) {
        while (!words[i].empty() && words[i].back() == ',') {
          words[i].pop_back();
        }

        table.print_column(words[i]);
        info.push_back(table.retrieval);
        table.retrieval.clear();
      }

      // Output formatting
      if (!info.empty() && !info[0].empty()) {
        for (int i = 0; i < info[0].size(); i++) {
          for (int j = 0; j < info.size(); j++) {
            std::cout << info[j][i];
            if (j != info.size() - 1)
              std::cout << "|";
            else
              std::cout << "\n";
          }
        }
      }
    } else {
      // The table name is always the last word
      std::string table_name = words[words.size() - 5];
      // FIX 1: Fetch the table ONCE, outside the loop!
      Table table = db.get_table(table_name);

      if (!table.is_valid()) {
        std::cerr << "Table not found!" << std::endl;
        return 0;
      }

      std::vector<std::vector<std::string>> info;

      for (int i = 1; i <= words.size() - 7; i++) {
        while (!words[i].empty() && words[i].back() == ',') {
          words[i].pop_back();
        }

        table.print_column(words[i]);
        info.push_back(table.retrieval);
        table.retrieval.clear();
      }

      std::string cond_col_match =
          words.back().substr(1, words.back().size() - 2);
      std::string cond_col = words[words.size() - 3];
      table.print_column(cond_col);
      std::vector<std::string> cond_col_data = table.retrieval;
      table.retrieval.clear();

      // Output formatting
      if (!info.empty() && !info[0].empty()) {
        for (int i = 0; i < info[0].size(); i++) {
          if (cond_col_data[i] != cond_col_match)
            continue;
          for (int j = 0; j < info.size(); j++) {
            std::cout << info[j][i];
            if (j != info.size() - 1)
              std::cout << "|";
            else
              std::cout << "\n";
          }
        }
      }
    }
  }

  return 0;
}