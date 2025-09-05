#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include <boost/asio.hpp>
#include <boost/asio/random_access_file.hpp>

#include <cstring>
#include <thread>
#include <vector>
#include <fstream>
#include <chrono>
#include <random>
#include <string_view>
#include <span>
#include <utility>

using namespace std;
using namespace boost::asio;

struct request {
    size_t anchor = 0;
    size_t offset = 0;
    size_t window_height = 16;
    vector<unsigned> sort_columns;
    vector<unsigned> column_widths;
};

struct answer {
    vector<char> characters;
    // cell texts in {buffer.data() + cell[c], buffer.data() + cell[c + 1]}
    // first row is header
    vector<unsigned> cells;
    unsigned width = 0, height = 0, position = 0, total = 0;
};

struct comma_separate_values {
    random_access_file file;
    random_access_file index_file;
    vector<char> read_buffers[2];

    request current_request;
    answer current_answer;
    // Quickselect is only O(n) if accessing a random half of elements only 
    // takes half the time. It might be necessary to copy elements to extra 
    // files for O(n). But without it's still only O(n log n).
};

struct parse_state {
    size_t column = 0, row = 0, cell_character = 0, file_byte = 0;
    bool escaped = false, quoted = false;
    char cell_delimiter = ',', row_delimiter = '\n';
};

enum class parse_result {
    again, cell_done, row_done,
};

pair<unsigned, parse_result> next_cell(
    parse_state &state, span<char> &characters, span<const char> &bytes
) {
    unsigned characters_read = 0;
    parse_result done = parse_result::again;
    while (!bytes.empty()) {
        auto b = bytes[0];
        bytes = bytes.subspan(1);
        state.file_byte++;
        if (b == state.cell_delimiter) {
            state.column++;
            state.cell_character = 0;
            done = parse_result::cell_done;
            break;

        } else if (b == state.row_delimiter) {
            state.column = 0;
            state.cell_character = 0;
            state.row++;
            done = parse_result::row_done;
            break;
            
        } else if (characters.empty()) {
            break;

        } else {
            characters[0] = b;
            characters = characters.subspan(1);
            state.cell_character++;
            characters_read++;
        }
        // TODO: escaping and quotes
    }
    return {characters_read, done};
}

int main(int argc, char *argv[]) {
    if (argc > 2) {
        if (strcmp(argv[1], "--make_test_data") == 0) {
            minstd_rand rand;
            ofstream file(argv[2]);
            char buffer[1024 * 8];
            file.rdbuf()->pubsetbuf(buffer, size(buffer));
            file << 
                "ID, Time, Flags, Host, Process, "
                "Thread, Levels, Message" << endl;
            auto base_time = std::chrono::time_point<std::chrono::system_clock>(
                std::chrono::seconds(1756842237)
            );
            for (int i = 0; i < 10'000'000; i++) {
                file << 
                    std::dec << i << ", " << 
                    base_time + std::chrono::milliseconds(rand()) << ", ";
                if (rand() % 2)
                    file << "Pool ";
                if (rand() % 2)
                    file << "Async ";
                file << ", ";

                static const char* hosts[]{
                    "localhost", "70.28.126.222", 
                    "84.140.123.208", "214.61.226.212"
                };
                file << hosts[rand() % size(hosts)] << ", ";
                
                static const char* processes[]{
                    "Database", "Server", "Telemetry"
                };
                file << processes[rand() % size(processes)] << ", ";
                
                file << "0x" << std::hex << rand() << ", ";
                
                static const char* levels[]{
                    "Call", "Debug", "Info", "Warning", "Error", "Fatal"
                };
                file << levels[rand() % size(levels)] << ", ";
                
                unsigned length = (1 << (rand() % 10));
                while (length-- > 0) {
                    file << (char)('0' + rand() % 0x4E);
                }
                file << endl;
            }
            return 0;
        }
    }

    Fl_Window win(400, 200, "BraceYourselfViewer");
    struct MyTable : public Fl_Table_Row {
        answer view;
    
        MyTable(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h) {
            col_header(1);
            col_header_height(25);
            col_resize(1);
            row_header(0);
            end(); // signals the end of the widgets constructor
        }

        void replace_content(answer &new_view) {
            swap(view, new_view);
            rows(view.height + 2);
            cols(view.width);
            row_height_all(25);
            col_width_all(100);
        }

        void draw_cell(
            TableContext context, int row, int column, 
            int x, int y, int w, int h
        ) override {
            if (context == CONTEXT_COL_HEADER) {
                fl_draw_box(FL_THIN_UP_BOX, x, y, w, h, FL_GRAY);
                fl_color(FL_BLACK);
                fl_draw(
                    view.characters.data() + view.cells[column], 
                    view.cells[column + 1] - view.cells[column],
                    x, y + 25 / 2 + fl_height() / 2
                );

            } else if (context == CONTEXT_CELL) {
                fl_draw_box(FL_FLAT_BOX, x, y, w, h, FL_WHITE);
                fl_color(FL_BLACK);
                if (row == 0 || (unsigned)row >= view.height)
                    // top and bottom row are placeholders
                    return;
                // row 0 is header
                auto cell = row * view.width + column;
                // TODO: use fl_width to find out width of text
                fl_draw(
                    view.characters.data() + view.cells[cell], 
                    view.cells[cell + 1] - view.cells[cell],
                    x, y + 25 / 2 + fl_height() / 2
                );
            }
        }
    };

    MyTable table(0, 0, 400, 200);
    table.resizable(&table);
    win.resizable(&table);
    win.end();
    win.show(argc, argv);

    io_context context;
    random_access_file file(context, "big.csv", random_access_file::read_only);
    vector<char> buffer(1024 * 8);
    file.read_some_at(0, boost::asio::buffer(buffer));

    answer new_answer;
    new_answer.total = 1;
    new_answer.height = 1;
    new_answer.characters.resize(1024 * 8);
    new_answer.cells.push_back(0);
    new_answer.cells.push_back(0);
    parse_state state;
    span<char> characters = new_answer.characters;
    span<const char> bytes = buffer;
    unsigned width = 0;
    while (!bytes.empty() && !characters.empty()) {
        auto result = next_cell(state, characters, bytes);
        new_answer.cells.back() += result.first;
        if (result.second != parse_result::again) {
            new_answer.cells.push_back(new_answer.cells.back());
            width++;
        }
        if (result.second == parse_result::row_done) {
            new_answer.width = max(width, new_answer.width);
            width = 0;
            new_answer.total++;
            new_answer.height++;
        }
    }
    while (width++ < new_answer.width) {
        // fill up last row
        new_answer.cells.push_back(new_answer.cells.back());
    }

    table.replace_content(new_answer);

    jthread worker;
    auto work = make_work_guard(context);

    worker = jthread([&](){
        context.run();
    });

    // TODO: pass requests to io_context via co_spawn
    // and results to main thread via Fk::awake(function, void*).
    // Requests should not be queued. Instead, a new request is filed when none
    // is pending, or when a request finishes and one is pending.

    return Fl::run();
}
