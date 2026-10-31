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
 * Timing: our dylib links only libobjc, so its constructor runs BEFORE
 * CoreServices initialises and registers its ObjC classes — objc_getClass
 * returns NULL there (seen on device: FUXI_BYPASS_NO_CLASS). Instead the
 * constructor starts a poller thread that swizzles the moment the class
 * appears, which is still before lsd's main runs the recovery.
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
extern long write(int fd, const void *buf, unsigned long count);
extern void usleep(unsigned usec);
extern long pthread_create(unsigned long *t, const void *attr, void *(*fn)(void *), void *arg);
extern void pthread_exit(void *);

#define SHIM_LOG(s) write(2, s, sizeof(s) - 1)

static IMP orig_recover;
static IMP orig_dispatch_journal;

/* replacement: return nil/void immediately */
static id stub_recover(id self, SEL _cmd)
{
	(void)self; (void)_cmd;
	return (id)0;
}

static void swizzle(Class c)
{
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
}

static void *poller(void *arg)
{
	(void)arg;
	for (int i = 0; i < 3000; i++) {   /* up to ~30 s at 10 ms */
		Class c = (Class)objc_getClass("LSInstallProgressService");
		if (c) {
			swizzle(c);
			SHIM_LOG("FUXI_BYPASS_SWIZZLED\n");
			return (void *)0;
		}
		usleep(10000);
	}
	SHIM_LOG("FUXI_BYPASS_CLASS_TIMEOUT\n");
	return (void *)0;
}

__attribute__((constructor))
static void shim_init(void)
{
	SHIM_LOG("FUXI_BYPASS_LOADED\n");
	unsigned long t;
	if (pthread_create(&t, 0, poller, 0) != 0) {
		SHIM_LOG("FUXI_BYPASS_THREAD_FAIL\n");
		/* last resort: try once synchronously */
		Class c = (Class)objc_getClass("LSInstallProgressService");
		if (c) {
			swizzle(c);
			SHIM_LOG("FUXI_BYPASS_SWIZZLED\n");
		}
	}
	/* detached: we never join */
}
