#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
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

uint32_t get_table_root_page(std::ifstream &db,
                             const std::string &target_table) {
  db.seekg(103);
  uint16_t num_cells = read_integer(db, 2);

  for (unsigned int pt = 0; pt < num_cells; pt++) {
    db.seekg(108 + pt * 2);
    uint16_t celloff = read_integer(db, 2);

    db.seekg(celloff);
    read_var(db);
    read_var(db);

    uint64_t header_start = db.tellg();
    uint64_t size_of_header = read_var(db);

    uint64_t serial_type = read_var(db);
    uint64_t serial_name = read_var(db);
    uint64_t serial_tbl_name = read_var(db);
    uint64_t serial_rootpage = read_var(db);

    uint64_t type_size = (serial_type - 13) / 2;
    uint64_t name_size = (serial_name - 13) / 2;

    // type Type 1 = 1 byte (int8), Type 2 = 2 bytes (int16), Type 3 = 3 bytes
    uint64_t rootpage_size =
        (serial_rootpage >= 1 && serial_rootpage <= 4) ? serial_rootpage : 0;

    uint64_t data_start = header_start + size_of_header;

    db.seekg(data_start);
    std::string type_string(type_size, '\0');
    db.read(&type_string[0], type_size);

    db.seekg(data_start + type_size);
    std::string table_name(name_size, '\0');
    db.read(&table_name[0], name_size);

    if (type_string == "table" && table_name == target_table) {
      uint64_t tbl_name_size = (serial_tbl_name - 13) / 2;
      db.seekg(data_start + type_size + name_size + tbl_name_size);

      return read_integer(db, rootpage_size);
    }
  }
  return 0;
}

uint64_t count_table_rows(std::ifstream &db, uint32_t page_num,
                          uint16_t page_size) {
  uint64_t offset = (page_num - 1) * page_size;

  db.seekg(offset);
  uint8_t page_type;
  db.read(reinterpret_cast<char *>(&page_type), 1);

  db.seekg(offset + 3);
  uint16_t num_cells = read_integer(db, 2);

  if (page_type == 0x0D) {
    return num_cells;
  }

  uint64_t total_rows = 0;
  if (page_type == 0x05) {
    for (int i = 0; i < num_cells; i++) {
      db.seekg(offset + 12 + (i * 2)); // Interior headers are 12 bytes long
      uint16_t cell_ptr = read_integer(db, 2);

      db.seekg(offset + cell_ptr);
      uint32_t left_child_page = read_integer(db, 4);

      total_rows += count_table_rows(db, left_child_page, page_size);
    }

    // Interior pages have "Right-most pointer" at offset 8
    db.seekg(offset + 8);
    uint32_t rightmost_page = read_integer(db, 4);
    total_rows += count_table_rows(db, rightmost_page, page_size);
  }

  return total_rows;
}

int main(int argc, char *argv[]) {
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

  if (command.find("SELECT COUNT(*)") != std::string::npos) {
    std::string target_table = command.substr(command.find_last_of(' ') + 1);

    uint32_t root_page = get_table_root_page(database_file, target_table);

    if (root_page != 0) {
      uint64_t total_rows =
          count_table_rows(database_file, root_page, page_size);
      std::cout << total_rows << std::endl;
    } else {
      std::cerr << "Table not found!" << std::endl;
    }
  }

  return 0;
}