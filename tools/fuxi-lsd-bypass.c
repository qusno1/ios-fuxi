/* fuxi-lsd-bypass.c — DYLD_INSERT_LIBRARIES shim for lsd (vermeer).
 *
 * lsd's startup dispatches [_LSInstallProgressService performJournalRecovery]
 * synchronously; the recovery asks MobileGestalt, whose helper cannot finish
 * initialising on this hardware (no SEP/baseband), so lsd freezes inside
 * LSServerMain's dispatch_once before serving its database, and SpringBoard
 * waits forever. Our roots have no journal to recover anyway.
 *
 * The shim neuters the two recovery methods via method_setImplementation:
 *   -[_LSInstallProgressService performJournalRecovery]
 *   -[_LSInstallProgressService dispatchJournalledNotificationsToConnectedClients]
 *
 * Built by tools-bin/mkdylib.py (bare Mach-O, ad-hoc signed, no Apple
 * toolchain) and installed into lsd's job EnvironmentVariables as
 * DYLD_INSERT_LIBRARIES=/usr/local/lib/fuxi/libfuxi-lsd-bypass.dylib.
 */

typedef struct objc_object *id;
typedef struct objc_class *Class;
typedef struct objc_selector *SEL;
typedef id (*IMP)(id, SEL, ...);

extern id objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern void *class_getInstanceMethod(Class cls, SEL sel);
extern IMP method_getImplementation(void *method);
extern IMP method_setImplementation(void *method, IMP imp);
extern const char *class_getName(Class cls);
extern id objc_msgSend(id self, SEL op, ...);
extern void *objc_getProtocol(const char *name);
extern long write(int fd, const void *buf, unsigned long count);

typedef unsigned long u64;

__attribute__((used)) static char shim_log_buf[128];

#define SHIM_LOG(s) write(2, s, sizeof(s) - 1)

static IMP orig_recover;
static IMP orig_dispatch_journal;

/* replacement: return nil/void immediately */
static id stub_recover(id self, SEL _cmd)
{
	(void)self; (void)_cmd;
	return (id)0;
}

static Class find_cls(const char *name)
{
	return (Class)objc_getClass(name);
}

__attribute__((constructor))
static void shim_init(void)
{
	SHIM_LOG("FUXI_BYPASS_LOADED\n");
	Class c = find_cls("LSInstallProgressService");
	if (c == (Class)0) {
		SHIM_LOG("FUXI_BYPASS_NO_CLASS\n");
		return;   /* not lsd; nothing to do */
	}
	SEL s1 = sel_registerName("performJournalRecovery");
	SEL s2 = sel_registerName("dispatchJournalledNotificationsToConnectedClients");
	void *m1 = class_getInstanceMethod(c, s1);
	void *m2 = class_getInstanceMethod(c, s2);
	if (m1) {
		orig_recover = method_getImplementation(m1);
		method_setImplementation(m1, (IMP)stub_recover);
	}
	if (m2) {
		orig_dispatch_journal = method_getImplementation(m2);
		method_setImplementation(m2, (IMP)stub_recover);
	}
	SHIM_LOG("FUXI_BYPASS_SWIZZLED\n");
}
