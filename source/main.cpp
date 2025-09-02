#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_draw.H>

#include <boost/asio/io_context.hpp>

#include <cstring>
#include <thread>

using namespace std;
using namespace boost::asio;

int main(int argc, char *argv[]) {
    Fl_Window win(400, 200, "BraceViewer");
    class MyTable : public Fl_Table_Row {
    public:
        MyTable(int X, int Y, int W, int H) : Fl_Table_Row(X, Y, W, H) {
            rows(5);
            cols(3);
            col_header(1);
            col_header_height(25);
            row_header(0);
            row_height_all(25);
            col_width_all(100);
            end();
        }

        void draw_cell(
            TableContext context, int R, int C, int X, int Y, int W, int H
        ) override {
            static const char* headers[] = {"ID", "Name", "Status"};
            static const char* data[5][3] = {
                {"1", "Alice", "OK"},
                {"2", "Bob", "Fail"},
                {"3", "Carol", "OK"},
                {"4", "Dave", "Warn"},
                {"5", "Eve", "OK"}
            };

            switch (context) {
            case CONTEXT_COL_HEADER:
                fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, FL_GRAY);
                fl_color(FL_BLACK);
                fl_draw(headers[C], X+2, Y, W-4, H, FL_ALIGN_CENTER);
                break;
            case CONTEXT_CELL:
                fl_draw_box(FL_FLAT_BOX, X, Y, W, H, FL_WHITE);
                fl_color(FL_BLACK);
                fl_draw(data[R][C], X+2, Y, W-4, H, FL_ALIGN_LEFT);
                break;
            default:
                break;
            }
        }
    };

    MyTable* table = new MyTable(0, 0, 400, 200);
    table->resizable(table);
    win.resizable(table);
    win.end();
    win.show(argc, argv);

    io_context context;
    jthread worker([&](){
        context.run();
    });


    return Fl::run();
}
