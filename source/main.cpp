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

const size_t READ_SIZE = 1024 * 8;

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
    while (!bytes.empty() && !characters.empty()) {
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
        }

        characters[0] = b;
        characters = characters.subspan(1);
        state.cell_character++;
        characters_read++;
        
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

    bool operator==(const column_sort&) const = default;
};

struct request {
    size_t anchor = 0;
    size_t offset = 0;
    size_t window_height = 0;
    vector<column_sort> sort_columns;
    vector<unsigned> column_widths;

    bool operator==(const request&) const = default;
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
    awaitable<pair<answer, request>> query(request r, answer a);

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
) : file(executor, "big.csv", random_access_file::read_only) {
    read_buffer.resize(READ_SIZE);
}

awaitable<pair<answer, request>> comma_separate_values::query(
    request r, answer a
) {
    // TODO: reuse content of last request/answer

    unsigned width = 0;
    a.total = 1;
    a.height = 1;
    a.characters.resize(1024);
    a.cells.clear();
    a.cells.push_back(0);
    a.cells.push_back(0);
    parse_state state;
    span<char> characters = a.characters;
    // TODO: check for end of file
    while (
        co_await async_read_at(
            file, state.file_byte, buffer(read_buffer), use_awaitable
        ) &&
        a.height <= r.window_height
    ) {
        span<const char> bytes = read_buffer;
        while (
            !bytes.empty() && a.height <= r.window_height
        ) {
            if (characters.empty()) {
                auto size = a.characters.size();
                a.characters.resize(size * 2);
                characters = a.characters;
                characters = characters.subspan(size);
            }
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
    }
    while (width++ < a.width) {
        // fill up last row
        a.cells.push_back(a.cells.back());
    }

    co_return pair<answer, request>{a, r};
}

void request_update(
    io_context::executor_type executor, struct table &table, 
    comma_separate_values &file
);

struct table : public Fl_Table_Row {
    io_context::executor_type executor;
    comma_separate_values *file;
    answer view, last_answer;
    request next_request, last_request;
    bool busy = false;

    table(
        io_context::executor_type executor,
        int x, int y, int w, int h,
        comma_separate_values *file
    ) : Fl_Table_Row(x, y, w, h), executor(executor), file(file) {
        col_header(1);
        col_header_height(25);
        col_resize(1);
        row_header(0);
        end(); // signals the end of the widgets constructor
    }

    void replace_content(const answer &new_view) {
        view = new_view;
        rows(view.height + 2);
        cols(view.width);
        row_height_all(25);
        row_height(0, new_view.position * 25);
        row_height(
            new_view.height + 1, 
            (new_view.total - new_view.height - new_view.position) * 25
        );
    }

    void draw_cell(
        TableContext context, int row, int column, 
        int x, int y, int w, int h
    ) override {
        if (context == CONTEXT_RC_RESIZE) {
            next_request.window_height = this->h() / 25;
            request_update(executor, *this, *file);

        } else if (context == CONTEXT_COL_HEADER) {
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

void request_update(
    io_context::executor_type executor, table &table, 
    comma_separate_values &file
) {
    if (table.busy) {
        // last request hasn't returned yet
        return;
    }
    if (table.next_request == table.last_request) {
        // nothing to do
        return;
    }
    table.busy = true;
    table.last_request = table.next_request;
    co_spawn(
        executor, 
        file.query(std::move(table.last_request), std::move(table.last_answer)),
        [&, executor] (exception_ptr, pair<answer, request> result) {
            auto lambda = [&, result = std::move(result), executor] () mutable {
                table.last_answer = std::move(result.first);
                table.last_request = std::move(result.second);
                table.replace_content(table.last_answer);
                table.busy = false;
                request_update(executor, table, file);
            };
            Fl::awake(
                [](void *lambda_pointer) {
                    (*((decltype(lambda)*)lambda_pointer))();
                }, 
                new decltype(lambda)(std::move(lambda))
            );
        }
    );
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

    io_context context;

    comma_separate_values file(context.get_executor());

    ::table table(context.get_executor(), 0, 0, 400, 200, &file);
    table.resizable(&table);
    win.resizable(&table);
    win.end();
    win.show(argc, argv);

    jthread worker;
    auto work = make_work_guard(context);

    worker = jthread([&](){
        context.run();
    });

    return Fl::run();
}
