#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include <boost/asio.hpp>

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
        // TODO: some maniacs don't escape their newlines, this means that we
        // have to read ahead an entire row to determine the end of the current
        // Maybe this function should be inlined into the request processing
    }
    return {characters_read, done};
}

struct column_sort {
    unsigned column;
    bool ascending = true;
};

struct request {
    size_t anchor = 0;
    size_t offset = 0;
    size_t window_height = 16;
    vector<column_sort> sort_columns;
    vector<unsigned> column_widths;
};

struct answer {
    vector<char> characters;
    // cell texts in {buffer.data() + cell[c], buffer.data() + cell[c + 1]}
    // first row is header
    vector<unsigned> cells;
    unsigned width = 0, height = 0, position = 0, total = 0;
};

struct pivot {
    size_t byte_offset;
    size_t rank_lower_bound, rank_upper_bound;
    // to sort:
    // Pick first row as pivot and count how many rows are below and above until
    // the bounds are outside of the visible window.
    // Pick a new counter, e.g. by just iterating over rows until you find a
    // smaller/bigger one, depending on the side of the window.
    // Repeat until the top 
};

struct comma_separate_values {
    comma_separate_values(io_context::executor_type executor);
    awaitable<pair<answer, request>> async_query(request r, answer a);

    random_access_file file;
    vector<char> read_buffer;

    pivot sort_pivot; 
    // Need at least one pivot, but could have more for performance
    // Comparing two rows requires reading them, and they can have any length

    request current_request;
    answer current_answer;
    // Quickselect is only O(n) if accessing a random half of elements only 
    // takes half the time. It might be necessary to copy elements to extra 
    // files for O(n). But without it's still only O(n log n).
};

comma_separate_values::comma_separate_values(
    io_context::executor_type executor
) : file(executor) {}

awaitable<pair<answer, request>> comma_separate_values::async_query(
    request r, answer a
) {
    // TODO: reuse content of last request/answer
    file.open("big.csv", random_access_file::read_only);
    read_buffer.resize(1024 * 8);

    co_await async_read_at(file, 0, buffer(read_buffer), use_awaitable);

    a.total = 1;
    a.height = 1;
    a.characters.resize(1024 * 8);
    a.cells.push_back(0);
    a.cells.push_back(0);
    parse_state state;
    span<char> characters = a.characters;
    span<const char> bytes = read_buffer;
    unsigned width = 0;
    while (!bytes.empty() && !characters.empty()) {
        auto result = next_cell(state, characters, bytes);
        a.cells.back() += result.first;
        if (result.second != parse_result::again) {
            a.cells.push_back(a.cells.back());
            width++;
        }
        if (result.second == parse_result::row_done) {
            a.width = max(width, a.width);
            width = 0;
            a.total++;
            a.height++;
        }
    }
    while (width++ < a.width) {
        // fill up last row
        a.cells.push_back(a.cells.back());
    }

    co_return pair<answer, request>{a, r};
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

    comma_separate_values file(context.get_executor());
    
    co_spawn(
        context, file.async_query({}, {}), 
        [&] (exception_ptr, pair<answer, request> result) {
            auto lambda = [&, result = std::move(result)] () mutable { 
                table.replace_content(result.first); 
            };
            Fl::awake(
                [](void *lambda_pointer) {
                    (*((decltype(lambda)*)lambda_pointer))();
                }, 
                new decltype(lambda)(std::move(lambda))
            );
        }
    );

    jthread worker;
    auto work = make_work_guard(context);

    worker = jthread([&](){
        context.run();
    });

    return Fl::run();
}
