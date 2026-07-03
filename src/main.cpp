#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

uint64_t read_var(std::ifstream &database_file) {
  uint64_t readval = 0;
  uint8_t ip;

  for (int i = 0; i < 9; i++) { // Varints are max 9 bytes
    database_file.read(reinterpret_cast<char *>(&ip), 1);

    if (i == 8) {
      readval = (readval << 8) | ip;
      break;
    }

    // Otherwise, use 7 bits and check the flag
    bool has_next = (ip >> 7) == 1;
    ip &= 0x7F;                    // Correct 7-bit mask
    readval = (readval << 7) | ip; // Correct shift

    if (!has_next)
      break;
  }
  return readval;
}

int main(int argc, char *argv[]) {
  // REMOVED freopen: CodeCrafters reads your stdout. If you redirect it to a
  // text file, the tests will fail!
  // freopen("output.txt", "w", stdout);
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  if (argc != 3) {
    std::cerr << "Expected two arguments" << std::endl;
    return 1;
  }

  std::string database_file_path = argv[1];
  std::string command = argv[2];

  std::ifstream database_file(database_file_path, std::ios::binary);
  if (!database_file) {
    std::cerr << "Failed to open the database file" << std::endl;
    return 1;
  }

  // ---------------------------------------------------------
  // COMMAND: .dbinfo
  // ---------------------------------------------------------
  if (command == ".dbinfo") {
    database_file.seekg(16);
    char buffer[2];
    database_file.read(buffer, 2);
    unsigned short page_size = (static_cast<unsigned char>(buffer[1]) |
                                (static_cast<unsigned char>(buffer[0]) << 8));
    std::cout << "database page size: " << page_size << std::endl;

    database_file.seekg(103);
    database_file.read(buffer, 2);
    unsigned short num_tables = (static_cast<unsigned char>(buffer[1]) |
                                 (static_cast<unsigned char>(buffer[0]) << 8));
    std::cout << "number of tables: " << num_tables << std::endl;
  }

  // ---------------------------------------------------------
  // COMMAND: .tables
  // ---------------------------------------------------------
  else if (command == ".tables") {
    database_file.seekg(103);
    char buffer[2];
    database_file.read(buffer, 2);
    unsigned short num_cells = (static_cast<unsigned char>(buffer[1]) |
                                (static_cast<unsigned char>(buffer[0]) << 8));

    for (unsigned int pt = 0; pt < num_cells; pt++) {
      database_file.seekg(108 + pt * 2);
      database_file.read(buffer, 2);
      unsigned short celloff = (static_cast<unsigned char>(buffer[1]) |
                                (static_cast<unsigned char>(buffer[0]) << 8));

      database_file.seekg(celloff);

      read_var(database_file);
      read_var(database_file);

      uint64_t header_start = database_file.tellg();
      uint64_t size_of_header = read_var(database_file);

      uint64_t serial_type = read_var(database_file);
      uint64_t serial_name = read_var(database_file);
      uint64_t serial_tbl_name = read_var(database_file);

      uint64_t type_size = (serial_type - 13) / 2;
      uint64_t name_size = (serial_name - 13) / 2;
      uint64_t tbl_name_size = (serial_tbl_name - 13) / 2;

      uint64_t data_start = header_start + size_of_header;

      database_file.seekg(data_start);
      std::string type_string(type_size, '\0');
      database_file.read(&type_string[0], type_size);

      database_file.seekg(data_start + type_size + name_size);
      std::string table_name(tbl_name_size, '\0');
      database_file.read(&table_name[0], tbl_name_size);

      if (type_string == "table" && table_name != "sqlite_sequence") {
        std::cout << table_name << " ";
      }
    }
    std::cout << std::endl;
  }

  return 0;
}