/*
 * libusb synchronization on Microsoft Windows
 *
 * Copyright © 2010 Michael Plante <michael.plante@gmail.com>
 * Copyright © 2020 Chris Dickens <christopher.a.dickens@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libusbi.h"

void usbi_cond_init(usbi_cond_t *cond)
{
	cond->waiters = 0;
	cond->sem = CreateSemaphoreW(NULL, 0, 0x7fffffff, NULL);
	assert(cond->sem != NULL);
}

void usbi_cond_destroy(usbi_cond_t *cond)
{
	CloseHandle(cond->sem);
}

void usbi_cond_broadcast(usbi_cond_t *cond)
{
	LONG n = InterlockedExchange(&cond->waiters, 0);

	if (n > 0)
		ReleaseSemaphore(cond->sem, n, NULL);
}

int usbi_cond_timedwait(usbi_cond_t *cond,
	usbi_mutex_t *mutex, const struct timeval *tv)
{
	DWORD millis, r;

	millis = (DWORD)(tv->tv_sec * 1000L) + (tv->tv_usec / 1000L);
	/* round up to next millisecond */
	if (tv->tv_usec % 1000L)
		millis++;

	InterlockedIncrement(&cond->waiters);
	LeaveCriticalSection(mutex);
	r = WaitForSingleObject(cond->sem, millis);
	EnterCriticalSection(mutex);

	if (r == WAIT_OBJECT_0)
		return 0;
	/* timed out: unregister, unless a broadcast already counted us */
	if (InterlockedDecrement(&cond->waiters) < 0)
		InterlockedIncrement(&cond->waiters);
	return r == WAIT_TIMEOUT ? LIBUSB_ERROR_TIMEOUT : LIBUSB_ERROR_OTHER;
}
