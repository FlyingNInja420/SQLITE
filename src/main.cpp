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
// 1. LOW-LEVEL BYTE & RECORD UTILITIES
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
  if (serial_type >= 2 && serial_type <= 4)
    return serial_type; // 16, 24, 32-bit ints
  if (serial_type == 5)
    return 6; // 48-bit int
  if (serial_type == 6 || serial_type == 7)
    return 8; // 64-bit int / float
  return 0;
}

class DatabaseUtils {
public:
  std::ifstream &db;
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
      readval = (readval << 7) | (ip & 0x7F);
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

  int64_t read_signed_integer(int bytes) {
    int64_t result = 0;
    for (int i = 0; i < bytes; i++) {
      uint8_t byte;
      db.read(reinterpret_cast<char *>(&byte), 1);
      if (i == 0 && (byte & 0x80))
        result = ~0ULL;
      result = (result << 8) | byte;
    }
    return result;
  }

  std::vector<std::string> parse_record() {
    uint64_t header_start = db.tellg();
    uint64_t size_of_header = read_var();
    uint64_t bytes_read = db.tellg() - header_start;

    std::vector<uint64_t> serial_types;
    while (bytes_read < size_of_header) {
      uint64_t before = db.tellg();
      serial_types.push_back(read_var());
      bytes_read += (db.tellg() - before);
    }

    std::vector<std::string> columns;
    for (uint64_t serial : serial_types) {
      uint16_t data_size = decode_size(serial);
      if (serial >= 13 && serial % 2 == 1) {
        std::string text(data_size, '\0');
        db.read(&text[0], data_size);
        columns.push_back(text);
      } else if (serial >= 1 && serial <= 6) {
        columns.push_back(std::to_string(read_signed_integer(data_size)));
      } else if (serial == 8) {
        columns.push_back("0");
      } else if (serial == 9) {
        columns.push_back("1");
      } else {
        db.seekg(db.tellg() + data_size); // Skip unhandled types (Blobs/Nulls)
        columns.push_back("");
      }
    }
    return columns;
  }
};

class Index {
private:
  DatabaseUtils &db;
  uint32_t root_page;
  uint16_t page_size;

  void find_rowids(uint32_t page_num, const std::string &target_val,
                   std::vector<int64_t> &rowids) {
    uint64_t offset = (page_num - 1) * page_size;
    db.seekg(offset);

    uint8_t page_type;
    db.read(reinterpret_cast<char *>(&page_type), 1);
    db.seekg(offset + 3);
    uint16_t num_cells = db.read_integer(2);

    if (page_type == 0x0A) { // Leaf Index Page
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 8 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        db.read_var(); // Payload size
        std::vector<std::string> record = db.parse_record();
        if (record.empty())
          continue;

        std::string index_key = record[0];
        if (index_key == target_val) {
          rowids.push_back(std::stoll(record[1]));
        } else if (index_key > target_val) {
          break; // Optimal binary search exit
        }
      }
    } else if (page_type == 0x02) { // Interior Index Page
      bool followed_path = false;
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 12 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        uint32_t left_child = db.read_integer(4);
        db.read_var(); // Payload size
        std::vector<std::string> record = db.parse_record();
        if (record.empty())
          continue;

        std::string index_key = record[0];

        if (target_val <= index_key) {
          find_rowids(left_child, target_val, rowids);
          if (target_val == index_key && record.size() > 1) {
            rowids.push_back(std::stoll(record[1]));
          } else {
            followed_path = true;
            break;
          }
        }
      }

      if (!followed_path) {
        db.seekg(offset + 8);
        uint32_t rightmost = db.read_integer(4);
        find_rowids(rightmost, target_val, rowids);
      }
    }
  }

public:
  Index(DatabaseUtils &db_ref, uint32_t root, uint16_t size)
      : db(db_ref), root_page(root), page_size(size) {}

  bool is_valid() const { return root_page != 0; }

  std::vector<int64_t> search(const std::string &target_val) {
    std::vector<int64_t> rowids;
    if (root_page != 0)
      find_rowids(root_page, target_val, rowids);
    return rowids;
  }
};

class Table {
private:
  DatabaseUtils &db;
  uint32_t root_page;
  uint16_t page_size;
  std::string sql;
  int rowid_col_idx = -1;
  std::vector<std::string> col_names;

  void parse_schema_columns() {
    size_t start = sql.find('(');
    size_t end = sql.find_last_of(')');
    if (start == std::string::npos || end == std::string::npos)
      return;

    std::string columns_str = sql.substr(start + 1, end - start - 1);
    std::stringstream ss(columns_str);
    std::string col_def;
    int index = 0;

    while (std::getline(ss, col_def, ',')) {
      size_t first_char = col_def.find_first_not_of(" \t\n\r");
      if (first_char != std::string::npos)
        col_def = col_def.substr(first_char);

      size_t space_idx = col_def.find(' ');
      std::string name = (space_idx != std::string::npos)
                             ? col_def.substr(0, space_idx)
                             : col_def;

      std::string lower_col = col_def;
      std::transform(lower_col.begin(), lower_col.end(), lower_col.begin(),
                     ::tolower);
      if (lower_col.find("integer primary key") != std::string::npos) {
        rowid_col_idx = index;
      }

      col_names.push_back(name);
      index++;
    }
  }

  int get_column_index(const std::string &target_col) {
    for (size_t i = 0; i < col_names.size(); ++i) {
      if (col_names[i] == target_col)
        return i;
    }
    return -1;
  }

  void print_row_format(const std::vector<std::string> &record, int64_t rowid,
                        const std::vector<int> &target_indices,
                        std::vector<std::string> *to_load = nullptr) {
    if (to_load == nullptr) {
      for (size_t k = 0; k < target_indices.size(); k++) {
        int idx = target_indices[k];
        if (idx == rowid_col_idx) {
          std::cout << rowid;
        } else if (idx < record.size()) {
          std::cout << record[idx];
        }
        if (k < target_indices.size() - 1)
          std::cout << "|";
      }
      std::cout << std::endl;
    } else {
      std::vector<std::string> current_loader;
      for (size_t k = 0; k < target_indices.size(); k++) {
        int idx = target_indices[k];
        if (idx == rowid_col_idx) {
          current_loader.push_back(std::to_string(rowid));
        } else {
          current_loader.push_back(record[idx]);
        }
      }
      *to_load = current_loader;
    }
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

public:
  Table(DatabaseUtils &db_ref, uint32_t root, uint16_t size, std::string schema)
      : db(db_ref), root_page(root), page_size(size), sql(schema) {
    parse_schema_columns();
  }

  bool is_valid() const { return root_page != 0; }
  uint64_t count_rows() { return count_rows_recursive(root_page); }

  void full_table_scan(uint32_t page_num,
                       const std::vector<std::string> &target_cols,
                       std::vector<std::vector<std::string>> &load_it) {
    if (page_num == 0)
      page_num = root_page;

    std::vector<int> col_indices;
    for (const auto &name : target_cols)
      col_indices.push_back(get_column_index(name));

    uint64_t offset = (page_num - 1) * page_size;
    db.seekg(offset);

    uint8_t page_type;
    db.read(reinterpret_cast<char *>(&page_type), 1);
    db.seekg(offset + 3);
    uint16_t num_cells = db.read_integer(2);

    if (page_type == 0x05) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 12 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        uint32_t left_child = db.read_integer(4);
        uint64_t cell_key = db.read_var();

        full_table_scan(left_child, target_cols, load_it);
        return;
      }
      db.seekg(offset + 8);
      uint32_t rightmost = db.read_integer(4);
      full_table_scan(rightmost, target_cols, load_it);
    } else if (page_type == 0x0D) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 8 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        db.read_var();
        uint64_t row_id = db.read_var();

        std::vector<std::string> record = db.parse_record();
        load_it.push_back({"Pushed_nothing"});
        print_row_format(record, row_id, col_indices, &load_it.back());
        return;
      }
    }
  }

  void print_row_by_id(uint32_t page_num, int64_t target_rowid,
                       const std::vector<std::string> &target_cols) {
    if (page_num == 0)
      page_num = root_page;

    std::vector<int> col_indices;
    for (const auto &name : target_cols)
      col_indices.push_back(get_column_index(name));

    uint64_t offset = (page_num - 1) * page_size;
    db.seekg(offset);

    uint8_t page_type;
    db.read(reinterpret_cast<char *>(&page_type), 1);
    db.seekg(offset + 3);
    uint16_t num_cells = db.read_integer(2);

    if (page_type == 0x05) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 12 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        uint32_t left_child = db.read_integer(4);
        uint64_t cell_key = db.read_var();

        if (target_rowid <= static_cast<int64_t>(cell_key)) {
          print_row_by_id(left_child, target_rowid, target_cols);
          return;
        }
      }
      db.seekg(offset + 8);
      uint32_t rightmost = db.read_integer(4);
      print_row_by_id(rightmost, target_rowid, target_cols);
    } else if (page_type == 0x0D) {
      for (int i = 0; i < num_cells; i++) {
        db.seekg(offset + 8 + (i * 2));
        uint16_t cell_ptr = db.read_integer(2);
        db.seekg(offset + cell_ptr);

        db.read_var();
        uint64_t row_id = db.read_var();

        if (static_cast<int64_t>(row_id) == target_rowid) {
          std::vector<std::string> record = db.parse_record();
          print_row_format(record, row_id, col_indices);
          return;
        }
      }
    }
  }
};

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

      std::vector<std::string> record = db.parse_record();
      if (record.size() >= 5) {
        SchemaEntry entry;
        entry.type = record[0];
        entry.name = record[1];
        entry.tbl_name = record[2];
        entry.root_page = std::stoi(record[3]);
        entry.sql = record[4];
        entries.push_back(entry);
      }
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

  Index get_index_for_table(const std::string &target_table,
                            const std::string &target_col) {
    for (const auto &entry : parse_schema()) {
      if (entry.type == "index" && entry.tbl_name == target_table) {
        // Minor fix: Check if column name is strictly within the index creation
        // string
        if (entry.sql.find("(" + target_col + ")") != std::string::npos ||
            entry.sql.find(target_col) != std::string::npos) {
          return Index(db, entry.root_page, page_size);
        }
      }
    }
    return Index(db, 0, page_size);
  }
};

// ---------------------------------------------------------
// 5. MAIN ROUTER
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
    if (table.is_valid()) {
      std::cout << table.count_rows() << std::endl;
    } else {
      std::cerr << "Table not found!" << std::endl;
    }
  } else if (lower_command.find("select ") == 0) {
    // Simple SQL Parser
    std::vector<std::string> words;
    std::string cur;
    std::stringstream ss(command);

    while (std::getline(ss, cur, ' ')) {
      if (!cur.empty())
        words.push_back(cur);
    }

    // Handle string literals with spaces
    for (size_t i = 0; i < words.size(); i++) {
      if (words[i].front() == '\'' && words[i].back() != '\'') {
        for (size_t j = i + 1; j < words.size(); j++) {
          words[i] += " " + words[j];
          if (words[j].back() == '\'') {
            words.erase(words.begin() + i + 1, words.begin() + j + 1);
            break;
          }
        }
      }
    }

    int from_idx = -1;
    int where_idx = -1;
    for (size_t i = 0; i < words.size(); i++) {
      std::string w = words[i];
      std::transform(w.begin(), w.end(), w.begin(), ::tolower);
      if (w == "from")
        from_idx = i;
      if (w == "where")
        where_idx = i;
    }

    if (from_idx != -1) {
      std::string table_name = words[from_idx + 1];
      Table table = db.get_table(table_name);

      if (!table.is_valid()) {
        std::cerr << "Table not found!" << std::endl;
        return 0;
      }

      std::vector<std::string> target_cols;
      for (int i = 1; i < from_idx; i++) {
        std::string col_name = words[i];
        while (!col_name.empty() && col_name.back() == ',') {
          col_name.pop_back();
        }
        target_cols.push_back(col_name);
      }

      if (where_idx != -1) {
        std::string cond_col = words[where_idx + 1];
        std::string cond_col_match = words[where_idx + 3];

        // Strip quotes from the search term
        if (cond_col_match.front() == '\'' && cond_col_match.back() == '\'') {
          cond_col_match = cond_col_match.substr(1, cond_col_match.size() - 2);
        }

        Index index = db.get_index_for_table(table_name, cond_col);

        if (index.is_valid()) {
          // BINARY SEARCH: Fetch matching rowids from the index
          std::vector<int64_t> matching_rowids = index.search(cond_col_match);

          // FETCH: Look up those specific rowids in the actual table
          for (int64_t rowid : matching_rowids) {
            table.print_row_by_id(0, rowid, target_cols);
          }
        } else {
          std::cerr << "Full table scans not implemented in streaming "
                       "architecture. Index required."
                    << std::endl;
          std::vector<std::vector<std::string>> fullinfo;
          table.full_table_scan(0, target_cols, fullinfo);

          int cond_idx_target = -1;
          for (int i = 0; i < target_cols.size(); i++)
            if (target_cols[i] == cond_col)
              cond_idx_target = i;

          for (auto &v_str : fullinfo) {
            for (int i = 0; i < v_str.size() - 1; i++) {
              if (i == cond_idx_target && cond_col_match == v_str[i])
                continue;
              std::cout << v_str[i];
              if (i != v_str.size() - 1)
                std::cout << "|";
              else
                std::cout << "\n";
            }
          }
        }
      } else {
        std::cerr << "print without where\n";
        std::vector<std::vector<std::string>> fullinfo;
        table.full_table_scan(0, target_cols, fullinfo);
        for (auto &v_str : fullinfo) {
          for (int i = 0; i < v_str.size() - 1; i++)
            std::cout << v_str[i] << "|";
          std::cout << v_str.back() << "\n";
        }
      }
    }
  }
  return 0;
}