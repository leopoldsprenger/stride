#include <ncurses.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

struct Item { int id{}; char kind{'t'}; std::string title, detail, area, project, doDate, deadline, tags, status; int order{}; bool someday{}; };
struct Ref { int id{}; std::string name; };

class Store {
 public:
  explicit Store(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) throw std::runtime_error("cannot open database");
    exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;"); migrate();
  }
  ~Store() { sqlite3_close(db_); }
  void exec(const std::string& q) { char* err=nullptr; if(sqlite3_exec(db_,q.c_str(),nullptr,nullptr,&err)!=SQLITE_OK){std::string e=err?err:"sqlite error";sqlite3_free(err);throw std::runtime_error(e);} }
  std::vector<Ref> areas() { return refs("SELECT id,name FROM areas WHERE status='open' ORDER BY sort_order,name"); }
  std::vector<Ref> projects(bool archived=false) { return refs(std::string("SELECT id,name FROM projects WHERE status ")+(archived?"!='open'":"='open'")+" ORDER BY sort_order,name"); }
  std::vector<Item> items(const std::string& view, const std::string& tagFilter="", int scopeId=0, char scopeKind=0) {
    std::string today="date('now','localtime')", open="status='open'";
    if(view=="Logged Projects") return read("SELECT p.id,'p',p.name,p.description,COALESCE(a.name,''),'',COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',p.status,CAST(strftime('%s',p.completed_at) AS INTEGER) FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE p.status!='open' ORDER BY p.completed_at DESC");
    if(view=="Archived Areas") return read("SELECT a.id,'a',a.name,'','','','','','','',CAST(strftime('%s',a.completed_at) AS INTEGER) FROM areas a WHERE a.status!='open' ORDER BY a.completed_at DESC");
    std::string where;
    if(view=="Inbox") where="t."+open+" AND t.area_id IS NULL AND t.project_id IS NULL AND t.do_date IS NULL AND t.someday=0";
    else if(view=="Today") where="t."+open+" AND t.someday=0 AND (t.do_date<="+today+" OR t.deadline<="+today+")";
    else if(view=="Tomorrow") where="t."+open+" AND t.someday=0 AND (t.do_date=date('now','localtime','+1 day') OR t.deadline=date('now','localtime','+1 day'))";
    else if(view=="Upcoming") where="t."+open+" AND t.someday=0 AND t.do_date>"+today;
    else if(view=="Deadlines") where="t."+open+" AND t.someday=0 AND t.deadline IS NOT NULL";
    else if(view=="Anytime") where="t."+open+" AND t.someday=0 AND t.do_date IS NULL";
    else if(view=="Someday") where="t."+open+" AND t.someday=1";
    else if(view=="Logbook") where="t.status='done'";
    else if(scopeKind=='a') where="t."+open+" AND t.area_id="+std::to_string(scopeId);
    else if(scopeKind=='p') where="t."+open+" AND t.project_id="+std::to_string(scopeId);
    else where="0";
    if(!tagFilter.empty()) { for(auto& tag: split(tagFilter,',')) where+=" AND instr(','||replace(t.tags,' ','')||',',',"+escape(tag)+",')>0"; }
    std::string q=std::string("SELECT t.id,'t',t.title,")+(view=="Logbook"?"COALESCE(t.completed_at,'')":"t.notes")+",COALESCE(a.name,''),COALESCE(p.name,''),COALESCE(t.do_date,''),COALESCE(t.deadline,''),t.tags,t.status,t.sort_order FROM tasks t LEFT JOIN areas a ON a.id=t.area_id LEFT JOIN projects p ON p.id=t.project_id WHERE "+where+" ORDER BY "+(view=="Logbook"?"t.completed_at DESC":"t.sort_order,t.id");
    auto out=read(q);
    if(view=="Today"||view=="Tomorrow"||view=="Deadlines") {
      std::string pw=(view=="Today"?"(p.do_date<="+today+" OR p.deadline<="+today+")":view=="Tomorrow"?"(p.do_date=date('now','localtime','+1 day') OR p.deadline=date('now','localtime','+1 day'))":"p.deadline IS NOT NULL");
      auto ps=read("SELECT p.id,'p',p.name,p.description,COALESCE(a.name,''),'',COALESCE(p.do_date,''),COALESCE(p.deadline,''),'',p.status,p.sort_order FROM projects p LEFT JOIN areas a ON a.id=p.area_id WHERE p.status='open' AND "+pw+" ORDER BY p.sort_order,p.id"); out.insert(out.begin(),ps.begin(),ps.end());
    }
    return out;
  }
  std::vector<Item> projectItems(int projectId, const std::string& tags="") {
    auto out=items("",tags,projectId,'p');
    auto hs=read("SELECT h.id,'h',h.title,'','','','','','','open',h.sort_order FROM headings h WHERE h.project_id="+std::to_string(projectId)+" ORDER BY h.sort_order,h.id");
    out.insert(out.end(),hs.begin(),hs.end()); std::sort(out.begin(),out.end(),[](const Item&a,const Item&b){return a.order<b.order;}); return out;
  }
  void saveTask(Item t, int areaId, int projectId) {
    if(areaId||projectId) t.someday=(t.doDate=="someday");
    if(t.someday) t.doDate="";
    sqlite3_stmt*s=prep(t.id?"UPDATE tasks SET title=?,notes=?,tags=?,area_id=?,project_id=?,do_date=?,deadline=?,someday=? WHERE id=?":"INSERT INTO tasks(title,notes,tags,area_id,project_id,do_date,deadline,someday,sort_order) VALUES(?,?,?,?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM tasks),0))");
    bind(s,1,t.title);bind(s,2,t.detail);bind(s,3,t.tags);num(s,4,areaId);num(s,5,projectId);nullable(s,6,t.doDate);nullable(s,7,t.deadline);sqlite3_bind_int(s,8,t.someday?1:0);if(t.id)sqlite3_bind_int(s,9,t.id);step(s);
  }
  void addArea(const std::string&name){stmt("INSERT INTO areas(name,sort_order) VALUES(?,COALESCE((SELECT MAX(sort_order)+1 FROM areas),0))",{name});}
  void addProject(const std::string&n,const std::string&d,int area,const std::string&dd,const std::string&dl){sqlite3_stmt*s=prep("INSERT INTO projects(name,description,area_id,do_date,deadline,sort_order) VALUES(?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM projects),0))");bind(s,1,n);bind(s,2,d);num(s,3,area);nullable(s,4,dd);nullable(s,5,dl);step(s);}
  void addHeading(int p,const std::string&n){sqlite3_stmt*s=prep("INSERT INTO headings(project_id,title,sort_order) VALUES(?,?,COALESCE((SELECT MAX(sort_order)+1 FROM headings WHERE project_id=?),0))");sqlite3_bind_int(s,1,p);bind(s,2,n);sqlite3_bind_int(s,3,p);step(s);}
  void complete(const Item&i){std::string tab=i.kind=='p'?"projects":i.kind=='a'?"areas":"tasks";exec("UPDATE "+tab+" SET status='"+(i.kind=='p'?"completed":"done")+"',completed_at=strftime('%Y-%m-%dT%H:%M:%f','now','localtime') WHERE id="+std::to_string(i.id));}
  void cancel(const Item&i){exec("UPDATE "+std::string(i.kind=='p'?"projects":"areas")+" SET status='cancelled',completed_at=strftime('%Y-%m-%dT%H:%M:%f','now','localtime') WHERE id="+std::to_string(i.id));}
  void erase(const Item&i){exec("DELETE FROM "+std::string(i.kind=='p'?"projects":i.kind=='a'?"areas":"tasks")+" WHERE id="+std::to_string(i.id));}
  void reorder(const Item&i,int delta){std::string tab=i.kind=='p'?"projects":"tasks";exec("UPDATE "+tab+" SET sort_order=sort_order+"+std::to_string(delta)+" WHERE id="+std::to_string(i.id));}
  void moveToProject(int taskId,int projectId){sqlite3_stmt*s=prep("UPDATE tasks SET project_id=?,area_id=(SELECT area_id FROM projects WHERE id=?),do_date=NULL,someday=0 WHERE id=?");num(s,1,projectId);sqlite3_bind_int(s,2,projectId);sqlite3_bind_int(s,3,taskId);step(s);}
  void migrate(){
    exec("CREATE TABLE IF NOT EXISTS areas(id INTEGER PRIMARY KEY,name TEXT UNIQUE NOT NULL,status TEXT NOT NULL DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT 0);");
    exec("CREATE TABLE IF NOT EXISTS projects(id INTEGER PRIMARY KEY,name TEXT NOT NULL UNIQUE,area TEXT,description TEXT NOT NULL DEFAULT '',area_id INTEGER REFERENCES areas(id),do_date TEXT,deadline TEXT,status TEXT NOT NULL DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT 0);");
    exec("CREATE TABLE IF NOT EXISTS tasks(id INTEGER PRIMARY KEY,title TEXT NOT NULL,notes TEXT NOT NULL DEFAULT '',tags TEXT NOT NULL DEFAULT '',project_id INTEGER REFERENCES projects(id) ON DELETE SET NULL,area_id INTEGER REFERENCES areas(id) ON DELETE SET NULL,heading_id INTEGER,do_date TEXT,deadline TEXT,someday INTEGER NOT NULL DEFAULT 0,status TEXT NOT NULL DEFAULT 'open',completed_at TEXT,sort_order INTEGER NOT NULL DEFAULT 0,done INTEGER NOT NULL DEFAULT 0,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    exec("CREATE TABLE IF NOT EXISTS headings(id INTEGER PRIMARY KEY,project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,title TEXT NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0);");
    for(auto q:{"ALTER TABLE projects ADD COLUMN description TEXT NOT NULL DEFAULT ''","ALTER TABLE projects ADD COLUMN area_id INTEGER","ALTER TABLE projects ADD COLUMN do_date TEXT","ALTER TABLE projects ADD COLUMN deadline TEXT","ALTER TABLE projects ADD COLUMN status TEXT NOT NULL DEFAULT 'open'","ALTER TABLE projects ADD COLUMN completed_at TEXT","ALTER TABLE projects ADD COLUMN sort_order INTEGER NOT NULL DEFAULT 0","ALTER TABLE tasks ADD COLUMN done INTEGER NOT NULL DEFAULT 0","ALTER TABLE tasks ADD COLUMN area_id INTEGER","ALTER TABLE tasks ADD COLUMN heading_id INTEGER","ALTER TABLE tasks ADD COLUMN do_date TEXT","ALTER TABLE tasks ADD COLUMN deadline TEXT","ALTER TABLE tasks ADD COLUMN someday INTEGER NOT NULL DEFAULT 0","ALTER TABLE tasks ADD COLUMN status TEXT NOT NULL DEFAULT 'open'","ALTER TABLE tasks ADD COLUMN completed_at TEXT","ALTER TABLE tasks ADD COLUMN sort_order INTEGER NOT NULL DEFAULT 0"}) try{exec(q);}catch(...){}
    exec("INSERT OR IGNORE INTO areas(name) SELECT DISTINCT area FROM projects WHERE area IS NOT NULL AND area!=''; UPDATE projects SET area_id=(SELECT id FROM areas WHERE areas.name=projects.area) WHERE area_id IS NULL AND area IS NOT NULL; UPDATE tasks SET status=CASE WHEN done=1 THEN 'done' ELSE 'open' END WHERE status='open';");
  }
 private:
  sqlite3*db_{};
  sqlite3_stmt*prep(const std::string&q){sqlite3_stmt*s{};sqlite3_prepare_v2(db_,q.c_str(),-1,&s,nullptr);return s;}
  static std::string tx(sqlite3_stmt*s,int n){auto p=(const char*)sqlite3_column_text(s,n);return p?p:"";}
  static void bind(sqlite3_stmt*s,int n,const std::string&v){sqlite3_bind_text(s,n,v.c_str(),-1,SQLITE_TRANSIENT);} static void num(sqlite3_stmt*s,int n,int v){if(v)sqlite3_bind_int(s,n,v);else sqlite3_bind_null(s,n);} static void nullable(sqlite3_stmt*s,int n,const std::string&v){if(v.empty())sqlite3_bind_null(s,n);else bind(s,n,v);} void step(sqlite3_stmt*s){sqlite3_step(s);sqlite3_finalize(s);} void stmt(const std::string&q,const std::vector<std::string>&v){auto*s=prep(q);for(int i=0;i<(int)v.size();++i)bind(s,i+1,v[i]);step(s);}
  std::vector<Ref>refs(const std::string&q){std::vector<Ref>o;auto*s=prep(q);while(sqlite3_step(s)==SQLITE_ROW)o.push_back({sqlite3_column_int(s,0),tx(s,1)});sqlite3_finalize(s);return o;}
  std::vector<Item>read(const std::string&q){std::vector<Item>o;auto*s=prep(q);while(sqlite3_step(s)==SQLITE_ROW)o.push_back({sqlite3_column_int(s,0),tx(s,1).empty()?'t':tx(s,1)[0],tx(s,2),tx(s,3),tx(s,4),tx(s,5),tx(s,6),tx(s,7),tx(s,8),tx(s,9),sqlite3_column_int(s,10)});sqlite3_finalize(s);return o;}
  static std::vector<std::string>split(const std::string&s,char d){std::vector<std::string>o;size_t p=0,n;while((n=s.find(d,p))!=std::string::npos){if(n>p)o.push_back(s.substr(p,n-p));p=n+1;}if(p<s.size())o.push_back(s.substr(p));return o;} static std::string escape(std::string s){s.erase(remove(s.begin(),s.end(),' '),s.end());return s;}
};

class App {
 public: explicit App(Store&s):s_(s){} void run(){setlocale(LC_ALL,"");initscr();cbreak();noecho();keypad(stdscr,TRUE);start_color();use_default_colors();init_pair(1,COLOR_CYAN,-1);init_pair(2,COLOR_RED,-1);init_pair(3,COLOR_GREEN,-1);while(on_){load();draw();handle(getch());}endwin();}
 private:
  Store&s_;bool on_=true,sidebar_=true,group_=false;int pick_=0,view_=0,scope_=0;char scopeKind_=0;std::string tags_,hidden_;std::vector<Item>list_;std::vector<std::string>views_{"Inbox","Today","Upcoming","Anytime","Someday","Logbook"};
  static std::string today(){std::time_t t=std::time(nullptr);char b[11]{};std::strftime(b,sizeof(b),"%F",std::localtime(&t));return b;}
  std::string active()const{return hidden_.empty()?views_[view_]:hidden_;}
  std::string name()const{return scopeKind_=='p'?"Project":scopeKind_=='a'?"Area":active();}
  void load(){list_=scopeKind_=='p'?s_.projectItems(scope_,tags_):s_.items(active(),tags_,scope_,scopeKind_);pick_=std::clamp(pick_,0,std::max(0,(int)list_.size()-1));}
  void draw(){erase();int r,c;getmaxyx(stdscr,r,c);int off=sidebar_?27:0;if(sidebar_){attron(A_BOLD|COLOR_PAIR(1));mvprintw(1,2,"◉ STRIDE");attroff(A_BOLD|COLOR_PAIR(1));mvprintw(3,2,"FOCUS");for(int i=0;i<(int)views_.size();++i){if(!scopeKind_&&i==view_)attron(A_REVERSE);mvprintw(5+i,2,"%s %s",i==0?"▣":"○",views_[i].c_str());if(!scopeKind_&&i==view_)attroff(A_REVERSE);}int y=13;mvprintw(y++,2,"AREAS");for(auto&a:s_.areas()){mvprintw(y++,3,"◇ %s",a.name.c_str());for(auto&p:s_.projects())if(y<r-4)mvprintw(y++,5,"▹ %s",p.name.c_str());if(y>=r-4)break;}mvprintw(r-2,2,"f find  b hide");}
    attron(A_BOLD);mvprintw(1,off+3,"%s%s",name().c_str(),group_?" · grouped":"");attroff(A_BOLD);if(!tags_.empty()){attron(A_DIM);mvprintw(1,c-22,"tags: %s",tags_.c_str());attroff(A_DIM);}mvhline(2,off+2,ACS_HLINE,c-off-4);std::string last;int y=4;for(int i=0;i<(int)list_.size()&&y<r-4;++i){auto&x=list_[i];std::string group=group_?(x.area.empty()?(x.project.empty()?"Inbox":x.project):(x.project.empty()?x.area:x.area+" / "+x.project)):(active()=="Upcoming"?x.doDate:active()=="Deadlines"?x.deadline:active()=="Logbook"?(x.detail.size()>=10?x.detail.substr(0,10):"Earlier"):"");if(!group.empty()&&group!=last){attron(A_BOLD|A_DIM);mvprintw(y++,off+3,"%s",group.c_str());attroff(A_BOLD|A_DIM);last=group;}if(i==pick_)attron(A_REVERSE);if(x.kind=='h'){attron(A_BOLD);mvprintw(y,off+4,"— %s",x.title.c_str());attroff(A_BOLD);}else{if(x.status!="open")attron(A_DIM);if(x.deadline!=""&&x.deadline<=today())attron(COLOR_PAIR(2));mvprintw(y,off+4,"%s %s",x.kind=='p'?"◇":x.kind=='a'?"◈":"○",x.title.c_str());attroff(COLOR_PAIR(2));if(x.status!="open")attroff(A_DIM);if(!x.area.empty()||!x.project.empty()){attron(A_DIM);mvprintw(y,c-24,"%s%s%s",x.area.c_str(),x.project.empty()?"":" / ",x.project.c_str());attroff(A_DIM);}}if(i==pick_)attroff(A_REVERSE);++y;}if(list_.empty()){attron(A_DIM);mvprintw(5,off+4,"Nothing here. n captures a next action.");attroff(A_DIM);}attron(A_DIM);mvprintw(r-2,off+3,"j/k select  J/K order  n task  N heading  p project  a area  f find  T tags");attroff(A_DIM);refresh();}
  std::string field(WINDOW*w,int y,const char*l,const std::string&v=""){mvwprintw(w,y,2,"%s",l);echo();curs_set(1);char b[240]{};mvwprintw(w,y,17,"%s",v.c_str());wmove(w,y,17);wgetnstr(w,b,230);noecho();curs_set(0);return b[0]?b:v;}
  void taskForm(std::optional<Item>e={}){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(15,68,(r-15)/2,(c-68)/2);box(w,0,0);Item t=e.value_or(Item{});mvwprintw(w,1,2,t.id?"EDIT TASK":"NEW TASK");t.title=field(w,3,"Title",t.title);t.detail=field(w,4,"Description",t.detail);t.tags=field(w,5,"Tags",t.tags);t.doDate=field(w,6,"Do date",t.doDate);t.deadline=field(w,7,"Deadline",t.deadline);std::string area=field(w,8,"Area",t.area);std::string project=field(w,9,"Project",t.project);mvwprintw(w,12,2,"Use YYYY-MM-DD; do date 'someday' parks it. Enter saves.");wrefresh(w);int a=0,p=0;for(auto&x:s_.areas())if(x.name==area)a=x.id;for(auto&x:s_.projects())if(x.name==project)p=x.id;if(wgetch(w)!=27&&!t.title.empty())s_.saveTask(t,a,p);delwin(w);}
  void addProject(){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(12,68,(r-12)/2,(c-68)/2);box(w,0,0);mvwprintw(w,1,2,"NEW PROJECT");auto n=field(w,3,"Name");auto d=field(w,4,"Description");auto ar=field(w,5,"Area");auto dd=field(w,6,"Do date");auto dl=field(w,7,"Deadline");int id=0;for(auto&a:s_.areas())if(a.name==ar)id=a.id;if(wgetch(w)!=27&&!n.empty())s_.addProject(n,d,id,dd,dl);delwin(w);}
  void addArea(){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(7,52,(r-7)/2,(c-52)/2);box(w,0,0);auto n=field(w,2,"Area");if(wgetch(w)!=27&&!n.empty())s_.addArea(n);delwin(w);}
  void find(){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(10,58,(r-10)/2,(c-58)/2);box(w,0,0);mvwprintw(w,1,2,"GO TO  Inbox Today Upcoming Anytime Someday Logbook Tomorrow Deadlines Logged Projects Archived Areas");auto q=field(w,3,"Find");hidden_.clear();for(int i=0;i<(int)views_.size();++i)if(views_[i].find(q)!=std::string::npos){view_=i;scopeKind_=0;}if(q=="Tomorrow"||q=="Deadlines"||q=="Logged Projects"||q=="Archived Areas"){hidden_=q;scopeKind_=0;}for(auto&p:s_.projects(true))if(p.name==q){scope_=p.id;scopeKind_='p';}for(auto&a:s_.areas())if(a.name==q){scope_=a.id;scopeKind_='a';}delwin(w);}
  void move(const Item&t){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(9,60,(r-9)/2,(c-60)/2);box(w,0,0);mvwprintw(w,1,2,"MOVE TO PROJECT (leave blank for Inbox)");int y=3;for(auto&p:s_.projects()){mvwprintw(w,y++,2,"◇ %s",p.name.c_str());if(y==7)break;}auto name=field(w,7,"Target");int id=0;for(auto&p:s_.projects())if(p.name==name)id=p.id;if(wgetch(w)!=27)s_.moveToProject(t.id,id);delwin(w);}
  void lifecycle(const Item&i){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(7,52,(r-7)/2,(c-52)/2);box(w,0,0);mvwprintw(w,1,2,"PROJECT / AREA: c complete, x cancel, d delete");int k=wgetch(w);delwin(w);if(k=='c')s_.complete(i);else if(k=='x')s_.cancel(i);else if(k=='d'){if(confirm("Delete permanently? y/N"))s_.erase(i);}}
  bool confirm(const std::string&message){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(5,44,(r-5)/2,(c-44)/2);box(w,0,0);mvwprintw(w,2,2,"%s",message.c_str());wrefresh(w);bool ok=wgetch(w)=='y';delwin(w);return ok;}
  void handle(int k){if(k=='q')on_=false;else if(k=='j'||k==KEY_DOWN)pick_=std::min(pick_+1,std::max(0,(int)list_.size()-1));else if(k=='k'||k==KEY_UP)pick_=std::max(0,pick_-1);else if(k=='h'||k==KEY_LEFT){scopeKind_=0;hidden_.clear();view_=(view_+views_.size()-1)%views_.size();}else if(k=='l'||k==KEY_RIGHT){scopeKind_=0;hidden_.clear();view_=(view_+1)%views_.size();}else if(k=='J'&&!list_.empty())s_.reorder(list_[pick_],1);else if(k=='K'&&!list_.empty())s_.reorder(list_[pick_],-1);else if(k=='b')sidebar_=!sidebar_;else if(k=='n')taskForm();else if(k=='p')addProject();else if(k=='a')addArea();else if(k=='N'&&scopeKind_=='p'){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(6,48,(r-6)/2,(c-48)/2);box(w,0,0);auto h=field(w,2,"Heading");if(wgetch(w)!=27&&!h.empty())s_.addHeading(scope_,h);delwin(w);}else if(k=='f')find();else if(k=='m'&&!list_.empty()&&list_[pick_].kind=='t')move(list_[pick_]);else if(k=='T'){int r,c;getmaxyx(stdscr,r,c);WINDOW*w=newwin(6,54,(r-6)/2,(c-54)/2);box(w,0,0);tags_=field(w,2,"Tags (comma)",tags_);wgetch(w);delwin(w);}else if(k=='A')group_=!group_;else if(k=='x'&&!list_.empty()){if(list_[pick_].kind=='t')s_.complete(list_[pick_]);else lifecycle(list_[pick_]);}else if(k=='d'&&!list_.empty()&&(active()=="Logbook"||active()=="Logged Projects"||active()=="Archived Areas"))s_.erase(list_[pick_]);else if(k=='e'&&!list_.empty()&&list_[pick_].kind=='t')taskForm(list_[pick_]);else if((k=='\n'||k==KEY_ENTER)&&!list_.empty()&&list_[pick_].kind=='p'){scope_=list_[pick_].id;scopeKind_='p';}else if(k=='?'){} }
};

int main(){try{const char*data=std::getenv("STRIDE_DATA_DIR");const char*home=std::getenv("HOME");auto dir=data?std::filesystem::path(data):std::filesystem::path(home?home:".")/".local/share/stride";std::filesystem::create_directories(dir);Store s((dir/"stride.db").string());App(s).run();}catch(const std::exception&e){std::cerr<<"stride: "<<e.what()<<'\n';return 1;}}
