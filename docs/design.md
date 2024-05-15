PathSpace
￼
Introduction
PathSpace is a new coordination language that draws inspiration from other coordination languages such as Linda as well as from other fields such as reactive programming and data oriented design.

Use Case
The main use case of PathSpace will be as a foundational component a larger system, for example a game engine, a visualization engine, a web based book reader or any application that needs a tree like structure for storing typed distributed data in a thread safe way. It’s meant to be a multi platform library supporting at least Windows, Linux, Android, OSX and iOS.

Internal Data
PathSpace is a class that can contain several other PathSpaces as well as several Data, every PathSpace or Data that it contains has a name. This forms a tree structure similar to a file system on a computer. The Data class can be seen as a queue of typed data and could for example contain a sequence of (int, float, int, A, std::string, lambda) where A is a user defined serializable class and lambda is a method that can generate a value when read.

Operations
The operations in the base language are insert/read/grab/transform, implemented as member functions of the PathSpace class.
* Insert: 
    * Insert Data or a PathSpace at a certain path. If the path does not exist it will be created. 
    * Supports batch operations by inserting a std::vector<T> while templating the insert<T> method on T.
    * Takes an optional capability object to determine if the user is allowed to insert at the specified path.
    * Takes an optional time to live object to determine the lifetime of the data.
    * The data inserted can be a lambda that is executed, if so the lambda should be encapsulated in an Execution class that can have data about the execution type. Properties for execution include if the lambda should execute right away, if it should be a lazy execution, if the value should be cached and only updated every n milliseconds.
    * Returns a std::expected<int, Error> with an int signifying how many items were inserted or an error describing what went wrong, if there is an Error, no items will have been inserted.
    * Takes a glob path which means it can support wildcard expansions that can enable insert to add items into many different paths at once.
    * Syntax:
        * std::expected<int, Error> PathSpace::insert(GlobPath, Value, optional<TimeToLive>, optional<Capability>)
    * Example:
        * std::expected<int, Error> val = space.insert("/test", 5); // insert 5 to “/test”
        * std::expected<int, Error> val = space.insert(“/*/abc”, 34); // insert 34 to, for example, “/test/abc” and “/second/abc”.
        * std::expected<int, Error> val = space.insert(“/a”, Execution{[](){return 54;}});
* Read:
    * Returns a copy of the value at the supplied path or Error if it could not be found, if for example the path did not exist or the front value had the wrong type.
    * Takes a block option that signifies how long to block/wait for data.
    * Does not support glob expressions.
    * Takes an optional capability object.
    * Syntax:
        * std::expected<T, Error> PathSpace::read<T>(ConcretePath, Block, optional<Capability>)
    * Example:
        * std::expected<int, Error> val = space.read<int>(“/test”); // returns the value at the path “/test” if it exists, else an Error
        * std::expected<int, Error> val = space.read<int>(“/test”, Block{30s}); // returns the value at the path “/test” if it exists, else wait 30s, if it has not been found by then return an error.
        * std::expected<int, Error> val = space.read<int>(“/test”, Block::infinity, Capabilities{“/**”}); // Has a capacity that allows reading any data, block forever waiting for the data, or until the PathSpace destructor is called
        * std::expected<PathSpace::json, Error> val = space.read<PathSpace::json>(“/test”); // Reads “/test” and it’s children back as a json object, if there are user objects that do not support conversion to json they become empty objects in the json.
        * std::expected<std::shared_ptr<PathSpace>, Error> val = space.read<std::shared_ptr<PathSpace>>(“/test”); // returns a shared_ptr to a PathSpace at path “/test”
* Grab: 
    * Same as read but pops the data instead of just returning a copy.
    * Syntax:
        * std::expected<T, Error> PathSpace::grab<T>(ConcretePath, Block, optional<Capability>)
* Stream
    * Loops over all the objects, when at the end it waits for more data.
    * For every object it executes a lambda
* Transform:
    * Returns either the number of items transformed or an Error.
    * Supports glob expressions for the path
    * Takes a lambda that can take either const &T or &T, the lambda is called for every value of type T on any matching path.
    * Takes a block option that signifies how long to block/wait for data.
    * Takes an optional capability object.
    * Syntax:
        * std::expected<int, Error> PathSpace::transform<T>(GlobPath, lambda, Block, optional<Capability>)

Blocking
It’s possible to send a blocking object to read/grab/transform instructing it to wait a certain amount of time for data to arrive it it is currently empty or non-existent.

Capability
The capability types are the PathSpace methods: read/grab/insert/transform. With more fine grained control over insert to control if for example executions/lambdas can be inserted. The capabilities are path based, controlling access over what parts of the tree can be used for what. Capabilities should be created via a factory so it does not become possibly for any function to grant itself any capability. A capability object can have control over multiple glob paths inside of it. 

User Created Data Types and Storage
The provided user data of type T to the insert method will be stores by copying them into a std::vector<T>. That vector will itself be stored inside a std::map<std::type_info*, std::any>. The process of appending one data to this structure is as follows:
1. Lock the space for writing
2. Find the right PathSpace in the tree based on the provided path.
3. Check if a matching std::any exists already based on &typeid(T).
4. If it does not exist, add a std::vector<T> to the map with the key &typeid(T).
5. Add the user data to std::any_cast<T&>(map[&typeid(T)]).push_back(t);
This method of storing objects can support built in types, stl types as well as user defined types.

Computations
It is possible to insert computations by inserting lambdas into the path space via the insert operator. The lambda is executed via read,grab or visit which will return the return value of the lambda.
* When run the lambda gets a parameter of the current path it is running on.
* By only issuing read commands to the lambda path it can run as long as needed. One example would be the main render loop, issued a new read every frame.
* If a grab is issued the lambda is removed from the space after running and returning one last value.
* Visit executes the lambda and visits the return value.
* Subscribe is issued every time the lambda has finished executing.
* The return value of the lambda will be inserted to the path on the lambda finishing its execution. Since the lambda cannot modify the path data itself this should work in a multithreaded context.
* Before a lambda is executed, the system should verify that the lambda has the necessary capabilities to perform its intended operations, such as read, write, or execute on the given paths.
* If a lambda has a time-to-live, it should be removed from any queues or storage when its time expires.
* If caching is enabled, the result of the lambda's execution can be stored and returned for subsequent reads until the cache expires.
* If the lambda returns a std::expected<T, Error> the error can be propagated to the caller of read/grab/etc.

Glob Expressions
Paths that point into the space can contain glob expressions, if the command is insert the data will be copied and inserted in any matching path, if the command is read or grab the command will use the lexically first matching path. A glob expression in insert cannot create paths since they have nothing to match against.

Serialisation
Serialisation will be supported out from the PathSpace into JSON in order to enable to do introspection and visualisation of the internal data of the path space or for export of performance data. For example to show a filesystem view of the tree in a GUI where individual data can be viewed. Will also support deserialisation for loading old state. Binary serialisation can also be done to for example create a state of the path space that can be serialised later for example for typical save/load functionality. This is achieved by specifying a json data type for read or grab, example: space.read<PathSpace::json>(“/test”)

Error Handling
The error handling in read/grab will be from the state of the std::expected as an Error struct on failure that can say for example that the operation failed due to the system being out of memory.

Multi-threading and thread safety
All the PathSpace operations will be thread safe. The threading will be done with a thread pool.

Memory
In order to avoid memory allocation calls to the operating system the PathSpace could have a pre-allocated pool of memory from the start. Perhaps some PathSpace tuple data could be marked as non essential somehow, if an out of memory situation occurs such data could be erased or flushed to disk.

External libraries
Data inserted should be serialised via the C++ library cereal into a binary stream of a std::vector<std::byte> at every path. JSON will be done via the nlohmann::json library. Catch2 will be used for testing.

Tools
CMake
Clang
GCC
VSCode
Ninja

Meta-data
Every PathSpace will have some metadata such as last time read, last time modified, how many times modified (same as version).

Logging Output
A log of every action can be produced. This is mostly for aiding in debugging.

Documentation
Documentation with examples and descriptions of the API will be provided on the GitHub page for the project.

Exception Handling
PathSpaces will not throw exceptions, all errors will be handled via the return type of the operations.

Load Balancing
TBD

Testing
All functionality will be tested via catch2 tests. By using a special CMake project that uses one .cpp file per operation to test.

Distributed Networking Considerations
Transaction Support
TBD
Multi-Version Consistency Control
TBD
Conflict-Free Replicated Data Types
TBD
Invariant-based Reasoning
TBD

Time-To-Live
(TTL, lifetime of data inserted)
Every path in a space will have a possible lifetime as determined on insert so that old data can automatically be cleaned.

Caching for Performance
A lambda execution can be set to cache the last value and only re-execute after a certain amount of time. Parts of the tree can use out of core extensions to store large data on the hard drive instead of RAM, the file system extension does the same, storing data in normal files on the filesystem that can be read from or modified. 

Fault Tolerance
TBD

Licensing
PathSpace will be under an open source license, likely GPL-3. I don’t mind if commercial entities use it as long as they contribute back code. In the unlikely event of commercial interest they can license it under a different license for a cost.

Back-Pressure Handling
TBD

Lazy Evaluation
TBD

Hot vs Cold Observables
TBD

GUI-based Monitoring Tool
TBD

Live Objects
TBD

Trellis
Lazy executions can be chained together via blocking operations waiting for new data, when one execution updates a value another listener to that value can execute. This creates a trellis like structure as described in the book mirror worlds book where values are fed from bottom processes and filtered up into higher order processes.

Default Paths
TBD

Benchmarks
Linda like tuple spaces have traditionally been seen as slow. To some degree this is due to the pattern matching required to extract data from a tuple. In PathSpace the slowest part will be traversing the path hierarchy, some speed ups have been thought of there (hash maps).

Version Migration Utilities
TBD

Metrics Collection
Will be possible to collect metrics on the data for the paths, how often they are accessed etc.

Data Integrity
Especially for the networking part some checking for data consistency will be needed.

Extendability
PathSpace will contain child path spaces of the same base type as the parent. They are implemented via std::shared_ptr<PathSpace> which means they can override the base class methods with their own implementation. This opens up the ability for parts of a PathSpace to have very specialized functionality. One space can be a view of the users filesystem via a PathSpace that implements filesystem operations via the read/insert/grab methods. Another space could be a scene graph of a game engine that deals with managing GPU state. Another space could be an abstraction over the operating system similar to the Unix philosophy of "everything is a file", enabling a platform independent way of for example reading the mouse position or creating a window to draw 3d models into.
The child spaces will need to implement the parent PathSpace methods as closely to the default implementation as possible, giving the same return values etc.

Reactive programming
The extendability of a PathSpace can be used to create a reactive data flow, for example by creating a child PathSpace that takes n other PathSpaces as input and combines their values. or a PathSpace that performs a transformation on any data from n Path Spaces. Each tuple can be seen as a data stream as long as they are alive, when they run out of items the stream dies. One example could be a lambda within the space that waits for the left mouse button to be pressed via space.read<int>(“/system/mouse/button_down”). In order to support subscriptions in a more water tight fashion it should also be possible to do something like: space.read<PathSpace::Subscription<int>>("/path”) which returns a Subscription object that can have callbacks added to it via ::addCallback(callback, optional<Capability>).

Distributed Data
Another specialization is to create a PathSpace child that can replicate a space over several computers over the network. Duplicating data where needed. May use Asia for networking. Like a live object.

Scene Graph
A scene graph of objects to be displayed by a 3d renderer. Mainly takes care of seamlessly interacting with the renderer to upload and display objects in the graph. Could also support data oriented design by having composable objects that store their properties in a list instead of objects in a list.

Vulcan Render
A renderer that integrates with the GPU via Vulcan. Makes it easy to upload and use meshes and shaders.

ISO Surface Generator
Having a ISO surface generator for just in time generation of geometry to display, taking LOD into account. It can have an internal path like …/generate/x_y_y that can be read to start the generation.

Operating System
The capabilities can be used to implement a user system and file system similar to Unix. When the initial PathSpace has been filled a process for the root user with full capability can be started by launching a lambda that executes a script. This is similar to how on login a bash session is started with a default script. This process can in turn launch other processes similar to systemd or cron.

Temporary Data Path
Will not write out any data for state saving and if a user logs out all their created data will be deleted. Will usually be at /tmp, with user dirs as /tmp/username.

Scripting
There will be support for manipulating a PathSpace through lua. Issuing an operator can then be done without having to reference a space object, like so: insert(“/a”, 5). But separate space objects could be created as variables in the script and could be used by the normal space_name.insert(… usage.

Command line
Using a live script interpreter a command line can be written.

Database front end
A ParhSpace could be an interpreter to a proper database library adding or changing data within it. For example mariadb or postgressql.

Compression
Compresses the input data seamlessly behind the scene on insert and decompresses it on read/grab.

Out-of-core
Similar to compression but can store the data on the hard drive instead of in RAM.