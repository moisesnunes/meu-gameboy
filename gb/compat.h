#ifndef _COMPAT_H_
#define _COMPAT_H_

/* Portabilidade Windows/POSIX — semáforos e diretório temporário */

#ifdef _WIN32

#include <windows.h>
#include <stdint.h>

/* Semáforo POSIX emulado com HANDLE do Windows */
typedef HANDLE sem_t;

static inline int sem_init(sem_t *s, int pshared, unsigned int value)
{
    (void)pshared;
    *s = CreateSemaphoreA(NULL, (LONG)value, 0x7fffffff, NULL);
    return *s ? 0 : -1;
}

static inline int sem_wait(sem_t *s)
{
    return WaitForSingleObject(*s, INFINITE) == WAIT_OBJECT_0 ? 0 : -1;
}

static inline int sem_trywait(sem_t *s)
{
    return WaitForSingleObject(*s, 0) == WAIT_OBJECT_0 ? 0 : -1;
}

static inline int sem_post(sem_t *s)
{
    return ReleaseSemaphore(*s, 1, NULL) ? 0 : -1;
}

static inline int sem_destroy(sem_t *s)
{
    return CloseHandle(*s) ? 0 : -1;
}

/* Diretório temporário */
static inline const char *compat_tmpdir(void)
{
    const char *t = getenv("TEMP");
    return t ? t : "C:\\Temp";
}

#else /* POSIX */

#include <semaphore.h>

static inline const char *compat_tmpdir(void)
{
    return "/tmp";
}

#endif /* _WIN32 */

#endif /* _COMPAT_H_ */
