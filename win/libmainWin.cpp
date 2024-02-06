#include "platformmfcontext.h"
#include "platformmfstream.h"
#include "platformstream.h"
#include "platformcontext.h"

#ifdef USE_MEDIA_FOUNDATION

Stream* createPlatformStream()
{
	return new PlatformMFStream();
}

Context* createPlatformContext()
{
	return new PlatformMFContext();
}
#else

Stream* createPlatformStream()
{
	return new PlatformStream();
}

Context* createPlatformContext()
{
	return new PlatformContext();
}

#endif