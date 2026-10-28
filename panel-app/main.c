/* The panel app: a home screen tile that shows PS4Mount's panel full screen in the in-app
 * web browser dialog, so the system browser keeps its own page (a WebKit jailbreak host).
 * The dialog has no bar to leave it: Back on the panel's first page asks PS4Mount to close
 * this app (/api/app/close). Without PS4Mount loaded it says so and ends. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#define PANEL_PORT 8770 /* WEB_PORT in src/web.h */
#define LOG_PATH "/data/ps4mount/panelapp.log"

/* SceWebBrowserDialogParam, 328 bytes: OpenOrbis has no type for it. */
typedef struct {
    OrbisCommonDialogBaseParam base;
    uint64_t size;
    int32_t mode; /* 1: the dialog's own layout, with a browser bar; 2: the rectangle below */
    int32_t user_id;
    const char *url;
    void *callback_init;
    uint16_t width, height, pos_x, pos_y;
    uint32_t parts;
    uint16_t header_width, header_x, header_y, pad0;
    uint32_t control;
    void *ime_param;
    void *webview_param;
    uint32_t animation;
    uint8_t reserved[202];
    uint16_t pad1;
} web_dialog_param;
_Static_assert(sizeof(web_dialog_param) == 328, "SceWebBrowserDialogParam is 328 bytes");

int sceWebBrowserDialogInitialize(void);
int sceWebBrowserDialogOpen(web_dialog_param *param);
int sceWebBrowserDialogUpdateStatus(void);
int sceWebBrowserDialogClose(void);
int sceWebBrowserDialogTerminate(void);

static void lg(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%ld ", (long)time(NULL));
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

static int user_id(void) {
    int32_t user = -1;
    sceUserServiceGetInitialUser(&user);
    return user;
}

/* PS4Mount answers on its port. */
static int panel_up(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PANEL_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int up = fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (fd >= 0)
        close(fd);
    return up;
}

static void message(const char *text) {
    OrbisMsgDialogParam param;
    OrbisMsgDialogUserMessageParam msg;
    memset(&param, 0, sizeof(param));
    memset(&msg, 0, sizeof(msg));
    param.baseParam.size = sizeof(param.baseParam);
    param.baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&param.baseParam);
    param.size = sizeof(param);
    param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    param.userId = user_id();
    msg.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = text;
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
    int rc = sceMsgDialogInitialize();
    if (rc < 0 || (rc = sceMsgDialogOpen(&param)) < 0) {
        lg("message dialog 0x%x: %s", (unsigned)rc, text);
        return;
    }
    while (sceMsgDialogUpdateStatus() == ORBIS_COMMON_DIALOG_STATUS_RUNNING)
        usleep(16000);
    sceMsgDialogTerminate();
}

/* The magic holds this block's address: it stays static. */
static web_dialog_param g_web __attribute__((aligned(16)));

static int show_panel(const char *url) {
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_WEB_BROWSER_DIALOG);
    int rc = sceWebBrowserDialogInitialize();
    memset(&g_web, 0, sizeof(g_web));
    g_web.base.size = sizeof(g_web.base);
    g_web.base.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)(uintptr_t)&g_web.base);
    g_web.size = sizeof(g_web);
    g_web.mode = 2;
    g_web.user_id = user_id();
    g_web.url = url;
    g_web.width = 1920;
    g_web.height = 1080;
    g_web.header_width = 1920;
    if (rc < 0 || (rc = sceWebBrowserDialogOpen(&g_web))) {
        lg("web dialog 0x%x", (unsigned)rc);
        return -1;
    }
    int st;
    while ((st = sceWebBrowserDialogUpdateStatus()) == ORBIS_COMMON_DIALOG_STATUS_RUNNING ||
           st == ORBIS_COMMON_DIALOG_STATUS_INITIALIZED)
        usleep(16000);
    sceWebBrowserDialogClose();
    sceWebBrowserDialogTerminate();
    return 0;
}

int main(void) {
    char url[48];
    int lang = 1;
    mkdir("/data/ps4mount", 0777);
    sceUserServiceInitialize(NULL);
    sceCommonDialogInitialize();
    sceSystemServiceHideSplashScreen();
    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &lang);
    int es = lang == 3 || lang == 20; /* lang_from_console in src/lang.c */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/?app=1", PANEL_PORT);
    if (!panel_up()) {
        lg("PS4Mount is not running");
        message(es ? "PS4Mount no está en marcha.\n\nCárgalo con GoldHEN (Payloader o BinLoader) y abre esta app otra vez."
                   : "PS4Mount is not running.\n\nLoad it with GoldHEN (Payloader or BinLoader), then open this app again.");
    } else if (show_panel(url)) {
        message(es ? "No se pudo mostrar el panel. Ábrelo en el navegador: http://127.0.0.1:8770"
                   : "The panel could not be shown. Open it in the web browser: http://127.0.0.1:8770");
    }
    sceSystemServiceLoadExec("exit", NULL);
    return 0;
}
