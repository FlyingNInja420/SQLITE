#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

uint64_t read_var(std::ifstream &database_file) {
  uint64_t readval = 0;
  uint8_t ip;
  for (int i = 0; i < 9; i++) {
    database_file.read(reinterpret_cast<char *>(&ip), 1);
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

uint32_t read_integer(std::ifstream &db, int bytes) {
  uint32_t result = 0;
  for (int i = 0; i < bytes; i++) {
    uint8_t byte;
    db.read(reinterpret_cast<char *>(&byte), 1);
    result = (result << 8) | byte;
  }
  return result;
}

// FIX 2: Properly return 0 for types that take no payload space
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

// ---------------------------------------------------------
// SCHEMA PARSING (DRY Principle Applied)
// ---------------------------------------------------------
// A struct to hold both pieces of data so we only parse the schema once
struct TableMetadata {
  uint32_t root_page = 0;
  std::string sql = "";
};

TableMetadata get_table_metadata(std::ifstream &db,
                                 const std::string &target_table) {
  TableMetadata meta;
  db.seekg(103);
  uint16_t num_cells = read_integer(db, 2);

  for (unsigned int pt = 0; pt < num_cells; pt++) {
    db.seekg(108 + pt * 2);
    uint16_t celloff = read_integer(db, 2);

    db.seekg(celloff);
    read_var(db); // Payload Size
    read_var(db); // RowID

    uint64_t header_start = db.tellg();
    uint64_t size_of_header = read_var(db);

    uint64_t serial_type = read_var(db);
    uint64_t serial_name = read_var(db);
    uint64_t serial_tbl_name = read_var(db);
    uint64_t serial_rootpage = read_var(db);
    uint64_t serial_sql = read_var(db);

    uint64_t type_size = (serial_type - 13) / 2;
    uint64_t name_size = (serial_name - 13) / 2;
    uint64_t rootpage_size =
        (serial_rootpage >= 1 && serial_rootpage <= 4) ? serial_rootpage : 0;
    uint64_t sql_size = (serial_sql - 13) / 2;

    uint64_t data_start = header_start + size_of_header;

    // Read Type and Name
    db.seekg(data_start);
    std::string type_string(type_size, '\0');
    db.read(&type_string[0], type_size);

    db.seekg(data_start + type_size);
    std::string table_name(name_size, '\0');
    db.read(&table_name[0], name_size);

    if (type_string == "table" && table_name == target_table) {
      uint64_t tbl_name_size = (serial_tbl_name - 13) / 2;

      // Read Root Page
      db.seekg(data_start + type_size + name_size + tbl_name_size);
      meta.root_page = read_integer(db, rootpage_size);

      // Read SQL String
      db.seekg(data_start + type_size + name_size + tbl_name_size +
               rootpage_size);
      meta.sql = std::string(sql_size, '\0');
      db.read(&meta.sql[0], sql_size);

      return meta;
    }
  }
  return meta;
}

int get_column_index(const std::string &sql, const std::string &target_col) {
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
    if (first_char != std::string::npos) {
      col_def = col_def.substr(first_char);
    }
    if (col_def.find(target_col) == 0)
      return index;
    index++;
  }
  return -1;
}

void read_col_data(std::ifstream &db, uint32_t page_num, uint16_t page_size,
                   int col_index) {
  uint64_t offset = (page_num - 1) * page_size;

  db.seekg(offset);
  uint8_t page_type;
  db.read(reinterpret_cast<char *>(&page_type), 1);

  db.seekg(offset + 3);
  uint16_t num_cells = read_integer(db, 2);

  // Leaf Page (Data Vault)
  if (page_type == 0x0D) {
    for (int i = 0; i < num_cells; i++) {
      db.seekg(offset + 8 + (i * 2));
      uint16_t cell_ptr = read_integer(db, 2);

      db.seekg(offset + cell_ptr);
      read_var(db); // Payload Size
      read_var(db); // RowID

      // FIX 1: The dynamic header reading trick
      uint64_t header_start = db.tellg();
      uint64_t size_of_header = read_var(db);
      uint64_t bytes_read = db.tellg() - header_start; // read by header

      std::vector<uint64_t> serial_types;

      while (bytes_read < size_of_header) {
        uint64_t before = db.tellg();
        serial_types.push_back(read_var(db));
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

        // codecrafters only string
        if (target_serial >= 13 && target_serial % 2 == 1) {
          std::string column_data(data_size, '\0');
          db.read(&column_data[0], data_size);
          std::cout << column_data << std::endl;
        } else if (target_serial == 0) {
          // It's a NULL value, print nothing (or handle appropriately)
        }
      }
    }
    return;
  }

  if (page_type == 0x05) {
    for (int i = 0; i < num_cells; i++) {
      db.seekg(offset + 12 + (i * 2));
      uint16_t cell_ptr = read_integer(db, 2);

      db.seekg(offset + cell_ptr);
      uint32_t left_child_page = read_integer(db, 4);

      read_col_data(db, left_child_page, page_size, col_index);
    }
    db.seekg(offset + 8);
    uint32_t rightmost_page = read_integer(db, 4);
    read_col_data(db, rightmost_page, page_size, col_index);
  }
}

uint64_t count_table_rows(std::ifstream &db, uint32_t page_num,
                          uint16_t page_size) {
  uint64_t offset = (page_num - 1) * page_size;

  db.seekg(offset);
  uint8_t page_type;
  db.read(reinterpret_cast<char *>(&page_type), 1);

  db.seekg(offset + 3);
  uint16_t num_cells = read_integer(db, 2);

  // Base Case: Leaf Page (0x0D)
  if (page_type == 0x0D) {
    return num_cells;
  }

  // Recursive Case: Interior Page (0x05)
  uint64_t total_rows = 0;
  if (page_type == 0x05) {
    for (int i = 0; i < num_cells; i++) {
      db.seekg(offset + 12 + (i * 2));
      uint16_t cell_ptr = read_integer(db, 2);

      db.seekg(offset + cell_ptr);
      uint32_t left_child_page = read_integer(db, 4);

      total_rows += count_table_rows(db, left_child_page, page_size);
    }

    db.seekg(offset + 8);
    uint32_t rightmost_page = read_integer(db, 4);
    total_rows += count_table_rows(db, rightmost_page, page_size);
  }

  return total_rows;
}

int main(int argc, char *argv[]) {
  // Required by CodeCrafters to ensure output isn't buffered
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  if (argc != 3)
    return 1;
  std::string database_file_path = argv[1];
  std::string command = argv[2];

  std::ifstream database_file(database_file_path, std::ios::binary);
  if (!database_file)
    return 1;

  database_file.seekg(16);
  uint16_t page_size = read_integer(database_file, 2);

  std::string lower_command = command;
  std::transform(lower_command.begin(), lower_command.end(),
                 lower_command.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (command == ".dbinfo") {
    std::cout << "database page size: " << page_size << std::endl;
    database_file.seekg(103);
    uint16_t num_tables = read_integer(database_file, 2);
    std::cout << "number of tables: " << num_tables << std::endl;

  } else if (command == ".tables") {
    database_file.seekg(103);
    uint16_t num_cells = read_integer(database_file, 2);

    for (unsigned int pt = 0; pt < num_cells; pt++) {
      database_file.seekg(108 + pt * 2);
      uint16_t celloff = read_integer(database_file, 2);

      database_file.seekg(celloff);
      read_var(database_file);
      read_var(database_file);

      uint64_t header_start = database_file.tellg();
      uint64_t size_of_header = read_var(database_file);
      uint64_t serial_type = read_var(database_file);
      uint64_t serial_name = read_var(database_file);

      uint64_t type_size = (serial_type - 13) / 2;
      uint64_t name_size = (serial_name - 13) / 2;

      database_file.seekg(header_start + size_of_header);
      std::string type_string(type_size, '\0');
      database_file.read(&type_string[0], type_size);

      database_file.seekg(header_start + size_of_header + type_size);
      std::string table_name(name_size, '\0');
      database_file.read(&table_name[0], name_size);

      if (type_string == "table" && table_name != "sqlite_sequence") {
        std::cout << table_name << " ";
      }
    }
    std::cout << std::endl;

  } else if (lower_command.find("select count(*)") != std::string::npos) {
    // fix this
    std::string target_table = command.substr(command.find_last_of(' ') + 1);
    TableMetadata meta = get_table_metadata(database_file, target_table);

    uint32_t root_page = meta.root_page;

    if (root_page != 0) {
      uint64_t total_rows =
          count_table_rows(database_file, root_page, page_size);
      std::cout << total_rows << std::endl;
    } else {
      std::cerr << "[ERROR] get_table_root_page returned 0!" << std::endl;
    }
  } else if (lower_command.find("select ") == 0) {
    std::vector<std::string> words;
    std::string cur;
    std::stringstream ss(lower_command);
    while (std::getline(ss, cur, ' '))
      words.push_back(cur);

    if (words.size() >= 4 && (words[2] == "from")) {
      std::string col_name = words[1];
      std::string table_name = words[3];

      TableMetadata meta = get_table_metadata(database_file, table_name);
      if (meta.root_page != 0) {
        int col_index = get_column_index(meta.sql, col_name);
        if (col_index != -1) {
          read_col_data(database_file, meta.root_page, page_size, col_index);
        } else {
          std::cerr << "Column not found in schema!" << std::endl;
        }
      } else {
        std::cerr << "Table not found!" << std::endl;
      }
    }
  }

  return 0;
}