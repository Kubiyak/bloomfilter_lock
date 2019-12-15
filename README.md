# bloomfilter_lock
Scalable read write locking with support for locking ranges of locks. I will add a more detailed README as I add more to the code.

The simple idea is to associate simple uint32_t values with resources that require locking and then use a mechanism inspired
by file/record locking to read or write lock one or more such resource simultaneously.

The scheme is locking order agnostic. Once the lock call returns, the caller owns locks of the requested types on the resources.

This is a refinement of the idea I published on bitbucket some time ago under https://bitbucket.org/Kubiyak/resource_lock


