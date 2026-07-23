#include <ncurses.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

struct Task { int id{}; std::string title, notes, tags, project, bucket; bool done{}; };
struct Project { int id{}; std::string name, area; };

class Store {
 public:
  explicit Store(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("cannot open database");
    exec("PRAGMA journal_mode=WAL;");
    exec("CREATE TABLE IF NOT EXISTS projects (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, area TEXT NOT NULL DEFAULT 'Personal');");
    exec("CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY, title TEXT NOT NULL, notes TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '', project_id INTEGER, bucket TEXT NOT NULL DEFAULT 'Inbox', done INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, FOREIGN KEY(project_id) REFERENCES projects(id));");
  }
  ~Store() { sqlite3_close(db_); }
  void exec(const char* sql) { char* err = nullptr; if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) { std::string e = err; sqlite3_free(err); throw std::runtime_error(e); } }
  std::vector<Project> projects() {
    std::vector<Project> out; sqlite3_stmt* s = prepare("SELECT id,name,area FROM projects ORDER BY area,name");
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back({sqlite3_column_int(s,0), text(s,1), text(s,2)}); sqlite3_finalize(s); return out;
  }
  std::vector<Task> tasks(const std::string& view) {
    std::string q = "SELECT t.id,t.title,t.notes,t.tags,COALESCE(p.name,''),t.bucket,t.done FROM tasks t LEFT JOIN projects p ON p.id=t.project_id WHERE ";
    if (view == "Logbook") q += "t.done=1";
    else if (view == "Anytime") q += "t.done=0 AND t.bucket IN ('Inbox','Anytime')";
    else if (view == "Inbox") q += "t.done=0 AND t.bucket='Inbox'";
    else q += "t.done=0 AND t.bucket='" + view + "'";
    q += " ORDER BY t.id DESC";
    std::vector<Task> out; sqlite3_stmt* s = prepare(q.c_str());
    while (sqlite3_step(s) == SQLITE_ROW) out.push_back({sqlite3_column_int(s,0),text(s,1),text(s,2),text(s,3),text(s,4),text(s,5),sqlite3_column_int(s,6)!=0}); sqlite3_finalize(s); return out;
  }
  void save(Task t, int projectId = 0) {
    sqlite3_stmt* s = prepare(t.id ? "UPDATE tasks SET title=?,notes=?,tags=?,project_id=?,bucket=? WHERE id=?" : "INSERT INTO tasks(title,notes,tags,project_id,bucket) VALUES(?,?,?,?,?)");
    bind(s,1,t.title); bind(s,2,t.notes); bind(s,3,t.tags); if (projectId) sqlite3_bind_int(s,4,projectId); else sqlite3_bind_null(s,4); bind(s,5,t.bucket); if (t.id) sqlite3_bind_int(s,6,t.id); sqlite3_step(s); sqlite3_finalize(s);
  }
  void done(int id) { sqlite3_stmt* s=prepare("UPDATE tasks SET done=1 WHERE id=?"); sqlite3_bind_int(s,1,id); sqlite3_step(s); sqlite3_finalize(s); }
  void move(int id, const std::string& bucket) { sqlite3_stmt* s=prepare("UPDATE tasks SET bucket=? WHERE id=?"); bind(s,1,bucket); sqlite3_bind_int(s,2,id); sqlite3_step(s); sqlite3_finalize(s); }
  void addProject(const std::string& name, const std::string& area) { sqlite3_stmt* s=prepare("INSERT OR IGNORE INTO projects(name,area) VALUES(?,?)"); bind(s,1,name); bind(s,2,area); sqlite3_step(s); sqlite3_finalize(s); }
 private:
  sqlite3* db_{};
  sqlite3_stmt* prepare(const char* q) { sqlite3_stmt* s{}; sqlite3_prepare_v2(db_,q,-1,&s,nullptr); return s; }
  static std::string text(sqlite3_stmt* s,int n) { auto p=reinterpret_cast<const char*>(sqlite3_column_text(s,n)); return p?p:""; }
  static void bind(sqlite3_stmt* s,int n,const std::string& v) { sqlite3_bind_text(s,n,v.c_str(),-1,SQLITE_TRANSIENT); }
};

class App {
 public:
  explicit App(Store& store): store_(store) {}
  void run() { setlocale(LC_ALL, ""); initscr(); cbreak(); noecho(); keypad(stdscr,TRUE); curs_set(0); start_color(); use_default_colors(); init_pair(1,COLOR_CYAN,-1); init_pair(2,COLOR_YELLOW,-1); init_pair(3,COLOR_GREEN,-1); while (running_) { refreshData(); draw(); handle(getch()); } endwin(); }
 private:
  Store& store_; bool running_=true, sidebar_=true; int selected_=0, section_=0; std::vector<Task> items_; std::vector<std::string> views_{"Inbox","Today","Upcoming","Anytime","Someday","Logbook"};
  void refreshData() { items_=store_.tasks(views_[section_]); selected_=std::clamp(selected_,0,std::max(0,(int)items_.size()-1)); }
  void draw() {
    erase(); int rows,cols; getmaxyx(stdscr,rows,cols); int side=sidebar_?24:0;
    if (sidebar_) { attron(A_BOLD|COLOR_PAIR(1)); mvprintw(1,2,"◉ STRIDE"); attroff(A_BOLD|COLOR_PAIR(1)); mvprintw(3,2,"FOCUS"); for(int i=0;i<(int)views_.size();++i){ if(i==section_) attron(A_REVERSE); mvprintw(5+i,2,"%s %s", i==0?"▣":"○",views_[i].c_str()); if(i==section_) attroff(A_REVERSE); } mvprintw(13,2,"PROJECTS"); int y=15; for(auto&p:store_.projects()){ mvprintw(y++,2,"  ◇ %s",p.name.c_str()); if(y>=rows-4)break;} mvprintw(rows-2,2,"b sidebar  ? help"); }
    attron(A_BOLD); mvprintw(1,side+3,"%s",views_[section_].c_str()); attroff(A_BOLD); attron(COLOR_PAIR(2)); mvprintw(1,cols-20,"%d open",(int)items_.size()); attroff(COLOR_PAIR(2));
    mvhline(2,side+2,ACS_HLINE,cols-side-4); if(items_.empty()) { attron(A_DIM); mvprintw(5,side+4,"Nothing here. Press n to capture a next action."); attroff(A_DIM); }
    for(int i=0;i<(int)items_.size() && i<rows-6;++i){ auto&t=items_[i]; int y=4+i; if(i==selected_) attron(A_REVERSE); attron(COLOR_PAIR(3)); mvprintw(y,side+4,"○"); attroff(COLOR_PAIR(3)); mvprintw(y,side+7,"%s",t.title.c_str()); if(!t.tags.empty()){ attron(A_DIM); mvprintw(y,cols-18,"#%s",t.tags.c_str()); attroff(A_DIM); } if(i==selected_) attroff(A_REVERSE); }
    attron(A_DIM); mvprintw(rows-2,side+3,"j/k select   n new   p project   ↵ details   e edit   x done   m move   q quit"); attroff(A_DIM); refresh();
  }
  std::string field(WINDOW* w,int y,const char* label,const std::string& value) { mvwprintw(w,y,2,"%s",label); wclrtoeol(w); echo(); curs_set(1); char b[240]{}; mvwprintw(w,y,18,"%s",value.c_str()); wmove(w,y,18); wgetnstr(w,b,230); noecho(); curs_set(0); return b[0]?b:value; }
  void form(std::optional<Task> existing={}) { int rows,cols; getmaxyx(stdscr,rows,cols); WINDOW*w=newwin(13,std::min(66,cols-4),(rows-13)/2,(cols-std::min(66,cols-4))/2); box(w,0,0); wbkgd(w,A_NORMAL); mvwprintw(w,1,2,existing?"EDIT TASK":"NEW TASK"); mvwprintw(w,10,2,"Enter saves • Esc cancels"); wrefresh(w); Task t=existing.value_or(Task{}); t.title=field(w,3,"Title",t.title); t.notes=field(w,4,"Description",t.notes); t.tags=field(w,5,"Tags",t.tags); t.bucket=field(w,6,"List",t.bucket.empty()?views_[section_]:t.bucket); auto ps=store_.projects(); std::string project=t.project; project=field(w,7,"Project",project); int pid=0; for(auto&p:ps)if(p.name==project)pid=p.id; int key=wgetch(w); if(key!=27 && !t.title.empty()) store_.save(t,pid); delwin(w); }
  void details() { if(items_.empty())return; auto&t=items_[selected_]; int rows,cols; getmaxyx(stdscr,rows,cols); WINDOW*w=newwin(12,std::min(68,cols-4),(rows-12)/2,(cols-std::min(68,cols-4))/2); box(w,0,0); mvwprintw(w,1,2,"TASK DETAILS"); mvwprintw(w,3,3,"%s",t.title.c_str()); mvwprintw(w,5,3,"%s",t.notes.empty()?"No description":t.notes.c_str()); mvwprintw(w,7,3,"#%s   %s",t.tags.empty()?"untagged":t.tags.c_str(),t.project.empty()?"No project":t.project.c_str()); mvwprintw(w,9,3,"e edit   x complete   m move   Esc close"); wrefresh(w); int k=wgetch(w); delwin(w); if(k=='e')form(t); else if(k=='x')store_.done(t.id); else if(k=='m')moveTask(t); }
  void projectForm() { int rows,cols; getmaxyx(stdscr,rows,cols); WINDOW*w=newwin(8,58,(rows-8)/2,(cols-58)/2); box(w,0,0); mvwprintw(w,1,2,"NEW PROJECT"); std::string name=field(w,3,"Name",""); std::string area=field(w,4,"Area","Personal"); mvwprintw(w,6,2,"Enter saves • Esc cancels"); wrefresh(w); if(wgetch(w)!=27&&!name.empty())store_.addProject(name,area); delwin(w); }
  void moveTask(const Task&t) { int rows,cols; getmaxyx(stdscr,rows,cols); WINDOW*w=newwin(8,42,(rows-8)/2,(cols-42)/2); box(w,0,0); mvwprintw(w,1,2,"MOVE TO LIST"); for(int i=0;i<5;++i)mvwprintw(w,3+i,2,"%d  %s",i+1,views_[i].c_str()); wrefresh(w); int k=wgetch(w); if(k>='1'&&k<='5')store_.move(t.id,views_[k-'1']); delwin(w); }
  void help() { int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(12,58,(r-12)/2,(c-58)/2);box(w,0,0);mvwprintw(w,1,2,"KEYBOARD");mvwprintw(w,3,2,"j/k ↑/↓  navigate     h/l  switch lists");mvwprintw(w,5,2,"n new task   p project    Enter details");mvwprintw(w,7,2,"e edit  x complete  m move  b sidebar  q quit");mvwprintw(w,9,2,"JetBrainsMono Nerd Font recommended for icons.");wrefresh(w);wgetch(w);delwin(w);}
  void handle(int k) { if(k=='q')running_=false; else if(k=='j'||k==KEY_DOWN)selected_=std::min(selected_+1,std::max(0,(int)items_.size()-1)); else if(k=='k'||k==KEY_UP)selected_=std::max(selected_-1,0); else if(k=='h'||k==KEY_LEFT)section_=(section_+views_.size()-1)%views_.size(),selected_=0; else if(k=='l'||k==KEY_RIGHT)section_=(section_+1)%views_.size(),selected_=0; else if(k=='b')sidebar_=!sidebar_; else if(k=='n')form(); else if(k=='p')projectForm(); else if((k=='\n'||k==KEY_ENTER))details(); else if(k=='e'&&!items_.empty())form(items_[selected_]); else if(k=='x'&&!items_.empty())store_.done(items_[selected_].id); else if(k=='m'&&!items_.empty())moveTask(items_[selected_]); else if(k=='?')help(); }
};

int main() { try { const char* home=std::getenv("HOME"); auto dir=std::filesystem::path(home?home:".")/".local/share/stride"; std::filesystem::create_directories(dir); Store store((dir/"stride.db").string()); App(store).run(); } catch(const std::exception&e) { std::cerr<<"stride: "<<e.what()<<'\n'; return 1; } }
