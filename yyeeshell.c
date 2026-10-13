#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#define MAX_TOKENS  256
#define MAX_ARGS    64
#define MAX_CMDS    64
#define MAX_PATHS   64
#define MAX_HISTORY 100

// pipe 裡的一段指令
typedef struct {
    char *argv[MAX_ARGS];
    char *in;   // < 的檔名，沒有就是 NULL
    char *out;  // > 的檔名，沒有就是 NULL
} Cmd;

char *paths[MAX_PATHS];   // search path
int npaths = 0;

char *history[MAX_HISTORY];
int nhist = 0;

volatile sig_atomic_t got_sigint = 0;   // 按了 Ctrl+C 就設成 1 (給 run 動畫用)

void print_error() {
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
}

// 在 < > | & 前後補空白，這樣 ls>out 也能切開
char *add_spaces(const char *line) {
    char *out = malloc(strlen(line) * 3 + 1); //*3最壞情況:每個字元都是運算子 每個都變成 3 個字元（空白、符號、空白）
    int j = 0;

    for (int i = 0; line[i] != '\0'; i++) {
        char c = line[i];
        if (c == '<' || c == '>' || c == '|' || c == '&') {
            out[j] = ' ';
            out[j + 1] = c;
            out[j + 2] = ' ';
            j = j + 3;
        } else {
            out[j] = c;
            j++;
        }
    }
    out[j] = '\0';
    return out;
}

// 把一段 token 分成 argv 和 < > 的檔名，有錯回傳 -1
int parse_cmd(char **tok, int n, Cmd *c) {
    int argc = 0;
    c->in = NULL;
    c->out = NULL;

    for (int i = 0; i < n; i++) {
        int is_in = (strcmp(tok[i], "<") == 0);
        int is_out = (strcmp(tok[i], ">") == 0);

        if (is_in || is_out) {
            // 後面要有檔名
            if (i + 1 >= n) {
                return -1;
            }
            char *file = tok[i + 1];
            if (strcmp(file, "<") == 0 || strcmp(file, ">") == 0) {
                return -1;
            }

            if (is_in) {
                if (c->in != NULL) {   // 重複的 <
                    return -1;
                }
                c->in = file;
            } else {
                if (c->out != NULL) {  // 重複的 >
                    return -1;
                }
                c->out = file;
            }
            i++;   // 跳過檔名
        } else {
            if (argc >= MAX_ARGS - 1) {
                return -1;
            }
            c->argv[argc] = tok[i];
            argc++;
        }
    }
    c->argv[argc] = NULL;   // execv 規定最後要放 NULL

    if (argc == 0) {   // 沒有指令，例如只打 > out
        return -1;
    }
    return 0;
}

int is_builtin(const char *s) {
    const char *names[] = {"exit", "cd", "path", "now", "help", "history", "yee", "art", "run"};
    int count = 9;

    for (int i = 0; i < count; i++) {
        if (strcmp(s, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

// ASCII 圖，最後放 NULL 當結尾
const char *ART_DINO[] = {
    "                 ___      RAWR!",
    "                / o \\    /",
    "               |  ___>",
    "               | |",
    "     _______   | |",
    "   _/  . .  \\__/ /",
    "  /  .   .  .   /",
    " ~~\\_  ____  __/",
    "     | |  | | |",
    "     |_|  |_|_|",
    NULL
};
const char *ART_CAT[] = {
    "  /\\_/\\",
    " ( o.o )",
    "  > ^ <   meow",
    NULL
};
const char *ART_ROCKET[] = {
    "     /\\",
    "    /  \\",
    "   | () |",
    "   |    |",
    "  /|    |\\",
    " /_|____|_\\",
    "    )  (",
    "   (    )",
    NULL
};
const char *ART_COFFEE[] = {
    "   ( (",
    "    ) )",
    "  ........",
    "  |      |]",
    "  \\      /",
    "   `----'",
    NULL
};

// color 是顏色碼，輸出到檔案時不加顏色
void print_art(const char **art, const char *color) {
    int is_terminal = isatty(STDOUT_FILENO);

    if (is_terminal) {
        printf("%s", color);
    }
    for (int i = 0; art[i] != NULL; i++) {
        printf("%s\n", art[i]);
    }
    if (is_terminal) {
        printf("\033[0m");   // 恢復原本顏色
    }
}

// ---- run：恐龍走過螢幕的動畫 ----
// 恐龍的身體 (8 行)，腳另外畫
const char *DINO_BODY[] = {
    "                 ___",
    "                / o \\",
    "               |  ___>",
    "               | |",
    "     _______   | |",
    "   _/  . .  \\__/ /",
    "  /  .   .  .   /",
    " ~~\\_  ____  __/",
};
// 兩種腳的姿勢，輪流畫就會像在走路
const char *LEGS_A[] = { "     | |  | | |", "     |_|  |_|_|" };
const char *LEGS_B[] = { "     / /  \\ \\ \\", "    /_/    \\_\\_\\" };

#define DINO_HEIGHT 10
#define RUN_STEPS   50

// 畫一格恐龍：x 是往右移幾格，legs 是腳的姿勢，poop = 1 就畫大便
void draw_dino(int x, const char **legs, int poop) {
    // 尾跡：恐龍走過的地方留下 . . . .
    char trail[RUN_STEPS + 1];
    for (int i = 0; i < x; i++) {
        if (i % 2 == 0) {
            trail[i] = '.';
        } else {
            trail[i] = ' ';
        }
    }
    trail[x] = '\0';

    // 身體 8 行
    for (int i = 0; i < 8; i++) {
        printf("\033[K%*s%s", x, "", DINO_BODY[i]);
        if (i == 1 && poop) {
            printf("    yee～");
        }
        printf("\n");
    }

    // 腳的兩行，第二行(地面)前面接尾跡
    // 大便畫在尾巴下面，剛好是腳左邊那 5 個空白的位置
    if (poop) {
        printf("\033[K%*s (@) %s\n", x, "", legs[0] + 5);
        printf("\033[K%s(___)%s\n", trail, legs[1] + 5);
    } else {
        printf("\033[K%*s%s\n", x, "", legs[0]);
        printf("\033[K%s%s\n", trail, legs[1]);
    }
}

void dino_run() {
    got_sigint = 0;
    printf("\033[?25l");   // 隱藏游標，不然會一直閃

    for (int x = 0; x < RUN_STEPS; x++) {
        // 偶數格用 A 腳，奇數格用 B 腳
        if (x % 2 == 0) {
            draw_dino(x, LEGS_A, 0);
        } else {
            draw_dino(x, LEGS_B, 0);
        }
        fflush(stdout);
        usleep(80000);     // 停 0.08 秒

        if (got_sigint) {  // 按了 Ctrl+C 就停下來，不大便了
            printf("\033[?25h");
            fflush(stdout);
            return;
        }
        printf("\033[%dA", DINO_HEIGHT);   // 游標往上 10 行，下一格蓋在同一個位置
    }

    // 走到終點：先站好停一下
    int end = RUN_STEPS - 1;
    draw_dino(end, LEGS_A, 0);
    fflush(stdout);
    usleep(500000);

    // 然後大便
    printf("\033[%dA", DINO_HEIGHT);
    draw_dino(end, LEGS_A, 1);

    printf("\033[?25h");   // 把游標顯示回來
    fflush(stdout);
}

// 一開始印出封面 (只有互動模式會呼叫)
void print_banner() {
    int is_terminal = isatty(STDOUT_FILENO);

    if (is_terminal) {
        printf("\033[32m");   // 綠色
    }
    printf("==========================================================\n");
    printf("                 ___\n");
    printf("                / o \\     RAWR!\n");
    printf("               |  ___>\n");
    printf("               | |         __   _______ _____ ____  _   _\n");
    printf("     _______   | |         \\ \\ / / ____| ____/ ___|| | | |\n");
    printf("   _/  . .  \\__/ /          \\ V /|  _| |  _| \\___ \\| |_| |\n");
    printf("  /  .   .  .   /            | | | |___| |___ ___) |  _  |\n");
    printf(" ~~\\_  ____  __/             |_| |_____|_____|____/|_| |_|\n");
    printf("     | |  | | |\n");
    printf("     |_|  |_|_|               yeesh - OS Project 1 Shell\n");
    printf("==========================================================\n");
    if (is_terminal) {
        printf("\033[0m");    // 恢復原本顏色
    }
    printf("輸入 help 查看指令，exit 離開\n\n");
    fflush(stdout);
}

// 印出 prompt：[時間] 使用者:目前目錄>
void print_prompt() {
    char cwd[512];    // 目前目錄 ex. /home/yee
    char tbuf[16];    // 時間字串 ex. 14:54:03

    time_t t = time(NULL);   // 取得現在的時間
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", localtime(&t));   // 轉成 時:分:秒

    // 取得目前目錄存進 cwd，拿不到(例如目錄被刪掉)就改成 ?
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "?");    //ex.[20:53:12] yee:?> ls
    }

    // 取得使用者名稱
    const char *user = getenv("USER");// 去環境變數找 USER，user 指向它的值
    if (user == NULL) {
        user = "user";
    }

    printf("[%s] %s:%s> ", tbuf, user, cwd);
    fflush(stdout);   // prompt 沒有換行，要自己 flush 才會馬上顯示
}

// 把指令存進 history (去掉換行，空白行不存)
void add_history(const char *line) {
    char *s = strdup(line);

    // 去掉最後的換行 不然的話打history他會印出一個然後空一行在印下一個
    int len = strlen(s);
    if (len > 0 && s[len - 1] == '\n') {
        s[len - 1] = '\0';
    }

    // 檢查是不是整行都是空白 空白就不用存在history裡面
    int all_space = 1;
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] != ' ' && s[i] != '\t') {
            all_space = 0;
            break;
        }
    }
    if (all_space) {
        free(s);
        return;
    }

    // 滿了就把最舊的丟掉 其他往前移一格
    if (nhist == MAX_HISTORY) {
        free(history[0]);
        for (int i = 1; i < MAX_HISTORY; i++) {
            history[i - 1] = history[i];
        }
        nhist--;
    }
    history[nhist] = s; //存起來
    nhist++; //位置+1
}

void run_builtin(char **argv) {
    int argc = 0;
    while (argv[argc] != NULL) {
        argc++;
    }

    if (strcmp(argv[0], "exit") == 0) {  //離開
        if (argc != 1) {
            print_error();
        } else {
            exit(0);
        }
    }
    else if (strcmp(argv[0], "cd") == 0) {
        if (argc != 2) {
            print_error();
        } else if (chdir(argv[1]) != 0) {  // chdir作為切到目錄
            print_error();
        }
    }
    else if (strcmp(argv[0], "path") == 0) {
        // 先清掉舊的，再存新的 (要 strdup，因為 argv 之後會被 free)
        for (int i = 0; i < npaths; i++) {
            free(paths[i]);
        }
        npaths = 0;
        for (int i = 1; i < argc && npaths < MAX_PATHS; i++) {
            paths[npaths] = strdup(argv[i]);
            npaths++;
        }
    }
    else if (strcmp(argv[0], "now") == 0) {
        char buf[64];
        time_t t = time(NULL);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S (%a)", localtime(&t));
        printf("%s\n", buf);
    }
    else if (strcmp(argv[0], "help") == 0) {
        printf("Built-in commands:\n");
        printf("  exit             離開 shell\n");
        printf("  cd <dir>         切換目錄\n");
        printf("  path [dir ...]   設定搜尋路徑\n");
        printf("  now              顯示目前日期時間\n");
        printf("  history          列出之前輸入過的指令\n");
        printf("  help             顯示這份說明\n");
        printf("  timeit <cmd>     執行指令並顯示花費時間\n");
        printf("  yee              召喚恐龍\n");
        printf("  run              恐龍走過螢幕\n");
        printf("  art [name]       畫圖：dino / cat / rocket / coffee\n");
        printf("Operators:  >  <  |  &\n");
    }
    else if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < nhist; i++) {
            printf("%4d  %s\n", i + 1, history[i]);
        }
    }
    else if (strcmp(argv[0], "yee") == 0) {
        print_art(ART_DINO, "\033[32m");   // 綠色
    }
    else if (strcmp(argv[0], "run") == 0) {
        if (isatty(STDOUT_FILENO)) {   // 只有終端機才播動畫
            dino_run();
        } else {
            print_art(ART_DINO, "");   // 導向檔案時只印一張圖
        }
    }
    else if (strcmp(argv[0], "art") == 0) {
        if (argc < 2) {
            printf("usage: art [dino | cat | rocket | coffee]\n");
        } else if (strcmp(argv[1], "dino") == 0) {
            print_art(ART_DINO, "\033[32m");
        } else if (strcmp(argv[1], "cat") == 0) {
            print_art(ART_CAT, "\033[33m");
        } else if (strcmp(argv[1], "rocket") == 0) {
            print_art(ART_ROCKET, "\033[36m");
        } else if (strcmp(argv[1], "coffee") == 0) {
            print_art(ART_COFFEE, "\033[35m");
        } else {
            printf("usage: art [dino | cat | rocket | coffee]\n");
        }
    }

    // 馬上印出來，不然輸出到檔案時順序會亂掉
    fflush(stdout);
}

// 在 search path 裡找程式，例如 ls -> /bin/ls，找不到回傳 NULL
char *find_exec(const char *name) {
    static char buf[1024];

    for (int i = 0; i < npaths; i++) {
        snprintf(buf, sizeof(buf), "%s/%s", paths[i], name);
        if (access(buf, X_OK) == 0) {   // 檔案存在而且可以執行  R_OK 可讀、W_OK 可寫、F_OK 只檢查存在   X_OK，因為一個檔案就算存在，沒有執行權限 execv 也會失敗
            return buf;
        }
    }
    return NULL;
}

// child 要做的事：接好 pipe 和重導向，然後 execv
void run_child(Cmd *cmd, int prev_fd, int fd[2]) {
    signal(SIGINT, SIG_DFL);   // child 按 Ctrl+C 要能結束

    // 從上一段 pipe 讀
    if (prev_fd != -1) {
        dup2(prev_fd, STDIN_FILENO);
        close(prev_fd);
    }
    // 寫到下一段 pipe
    if (fd[1] != -1) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);
    }

    // < 重導向
    if (cmd->in != NULL) {
        int f = open(cmd->in, O_RDONLY);
        if (f < 0) {
            print_error();
            _exit(1);
        }
        dup2(f, STDIN_FILENO);
        close(f);
    }
    // > 重導向
    if (cmd->out != NULL) {
        int f = open(cmd->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) {
            print_error();
            _exit(1);
        }
        dup2(f, STDOUT_FILENO);
        close(f);
    }

    // built-in 不能放在 pipe 裡
    if (is_builtin(cmd->argv[0])) {
        print_error();
        _exit(1);
    }

    char *path = find_exec(cmd->argv[0]);
    if (path != NULL) {
        execv(path, cmd->argv);
    }
    // 執行到這裡代表找不到或 execv 失敗
    print_error();
    _exit(1);
}

// 執行一個 job (可能有 |)，fork 出來的 pid 存進 pids，回傳 fork 了幾個
int run_pipeline(char **tok, int n, pid_t *pids) {
    Cmd cmds[MAX_CMDS];
    int ncmd = 0;
    int start = 0;

    // 用 | 切開，每一段交給 parse_cmd
    for (int i = 0; i <= n; i++) {
        if (i < n && strcmp(tok[i], "|") != 0) {
            continue;
        }
        // 到這裡代表遇到 | 或是到最後了
        if (ncmd >= MAX_CMDS) {
            print_error();
            return 0;
        }
        if (parse_cmd(&tok[start], i - start, &cmds[ncmd]) == -1) {
            print_error();
            return 0;
        }
        ncmd++;
        start = i + 1;
    }

    // 只有一個 built-in 的話，在 shell 自己這邊執行 (cd 才有用)
    if (ncmd == 1 && is_builtin(cmds[0].argv[0])) {
        if (cmds[0].in != NULL || cmds[0].out != NULL) {
            print_error();
        } else {
            run_builtin(cmds[0].argv);
        }
        return 0;
    }

    int count = 0;
    int prev_fd = -1;   // 上一段 pipe 的讀端

    for (int k = 0; k < ncmd; k++) {
        int fd[2] = {-1, -1};

        // 不是最後一段才需要開新的 pipe
        if (k < ncmd - 1) {
            if (pipe(fd) < 0) {
                print_error();
                break;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            print_error();
            break;
        }
        if (pid == 0) {
            run_child(&cmds[k], prev_fd, fd);   // 不會回來
        }

        // parent
        pids[count] = pid;
        count++;
        if (prev_fd != -1) {
            close(prev_fd);
        }
        if (fd[1] != -1) {
            close(fd[1]);   // 一定要關，不然下一段讀不到 EOF 會卡住
        }
        prev_fd = fd[0];    // 讀端留給下一段
    }

    if (prev_fd != -1) {
        close(prev_fd);
    }
    return count;
}

// 處理一整行：用 & 切成好幾個 job，全部開始跑之後再一起 wait
void process_line(const char *line) {
    char *s = add_spaces(line);  //先把指令多加空白像ls&pwd\n→ ls  &  pwd\n
    char *tok[MAX_TOKENS];
    int n = 0; //做有幾個指令的紀錄

    // 用空白切 token
    char *t = strtok(s, " \t\n");// 空白、tab（\t）、換行（\n）到這三種任何一個，就算一個單字結束了
    while (t != NULL) {
        if (n >= MAX_TOKENS) {
            print_error();
            free(s);
            return;
        }
        tok[n] = t;
        n++;
        t = strtok(NULL, " \t\n");
    }

    pid_t pids[MAX_TOKENS];
    int np = 0;
    int start = 0;

    // 用 & 切開，每一段交給 run_pipeline
    for (int i = 0; i <= n; i++) {   //ex.tok：[0]ls  [1]&  [2]pwd     n = 3
        if (i < n && strcmp(tok[i], "&") != 0) { //i = 0，tok[0] = "ls"，不是 &，continue  //i = 1，tok[1] = "&"，要處理 tok[0] 到 tok[0]
            continue;
        }
        // 空的 job 跳過，例如 ls & 最後面那個
        if (i > start) {
            np += run_pipeline(&tok[start], i - start, &pids[np]);//呼叫 run_pipeline(&tok[0], 1, &pids[0])
        }
        start = i + 1;
    }

    // 全部 fork 完再一起等，這樣才是平行執行
    for (int i = 0; i < np; i++) {
        while (waitpid(pids[i], NULL, 0) == -1 && errno == EINTR) {
            // 被 Ctrl+C 打斷就繼續等
        }//pid和行程表格數是有上限的 不做這個會被占用
    }
    free(s);
}

// shell 收到 Ctrl+C 時只換行，不會結束
void sigint_handler(int sig) {
    (void)sig;   // 沒用到，避免編譯警告
    got_sigint = 1;                 //讓 run 動畫知道要停
    write(STDOUT_FILENO, "\n", 1); //印換行
}

// 開頭是 timeit 就計時，不是就照常執行
void run_line(const char *line) {
    // 跳過前面的空白 防止打指令時前面不小心按到空白鍵進而讀不到指令
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    int is_timeit = 0;
    if (strncmp(p, "timeit", 6) == 0) {   //比較前6個字是否為timeit
        char next = p[6];
        if (next == ' ' || next == '\t' || next == '\n') {   //確認 timeit 後面是空白 不然 timeit123 也會被誤認
            is_timeit = 1;
        }
    }

    if (!is_timeit) {   //不是timeit就不用計時了
        process_line(line);
        return;
    }

    struct timespec start, end;  // 兩個空的時間
    clock_gettime(CLOCK_MONOTONIC, &start);
    process_line(p + 6);   //執行 timeit 後面的指令
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ms = (end.tv_sec - start.tv_sec) * 1000.0
              + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    fprintf(stderr, "[timeit] %.3f ms\n", ms);//不緩衝 馬上印出
}

int main(int argc, char *argv[]) {
    FILE *in = stdin;      // 預設從鍵盤讀
    int interactive = 1;   // 1 代表互動模式

    // 判斷是 batch 還是 interactive
    if (argc > 2) {               //參數不會大於2
        print_error();
        exit(1);
    }
    if (argc == 2) {
        in = fopen(argv[1], "r");         // 打開檔案，in 改成指向檔案
        if (in == NULL) {             // 打不開
            print_error();
            exit(1);
        }
        interactive = 0;// 不印 prompt
    }

    // 預設search path(因為execv他需要完整路徑，ex./bin/ls)
    paths[0] = strdup("/bin");      //查的(用 strdup 而不是直接放 “/bin”，因為之後 path 指令會 free 它，字串常數不能 free)
    npaths = 1;

    // 互動模式才印封面，batch 模式不能有多餘的輸出
    if (interactive) {
        print_banner();
    }

    // 設定 Ctrl+C 的處理方式
    // 沒有設 SA_RESTART，所以 getline / waitpid 被 Ctrl+C 打斷時會回傳 EINTR  //沒有這些的話打ctrl+c會換行前面不會顯示[18:56:36] yee:/home/yee這個樣子 會看起來當掉
    struct sigaction sa;// 準備一張設定表
    memset(&sa, 0, sizeof(sa));// 全部清成 0
    sa.sa_handler = sigint_handler;// 收到 Ctrl+C 時，去執行 sigint_handler
    sigaction(SIGINT, &sa, NULL);// 把設定表交給 OS

    char *line = NULL;
    size_t cap = 0;  //這兩行用在getline(&line, &cap, in)  NULL和0它會自己 malloc 一塊夠大的空間
        //一直執行 互動模式就輸入一個然後印出一行 getline是遇到ctrl+c就中斷從印
    while (1) {
        if (interactive) {
            print_prompt();
        }

        errno = 0;
        if (getline(&line, &cap, in) == -1) {
            if (errno == EINTR) {   // 被 Ctrl+C 打斷，重新印 prompt
                clearerr(in);       //把 FILE 的錯誤標記清掉
                continue;
            }
            break;                  // 讀到 EOF
        }

        add_history(line);
        run_line(line);
    }
    exit(0);
}