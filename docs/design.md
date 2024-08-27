# PathSpace
![PathSpace](images/PathSpace.jpeg)

## Introduction
PathSpace is a new coordination language that draws inspiration from other coordination languages such as Linda and other fields such as reactive programming and data oriented design.

## Use Case
The main use case of PathSpace is as a foundational component a larger system, for example a game engine, a visualization engine, a web based book reader or any application that needs a tree like structure for storing typed distributed data in a thread safe way. It’s meant to be a multi platform library supporting at least Windows, Linux, Android, OSX and iOS.

## Internal Data
PathSpace is a class that can contain several other PathSpaces as well as several pieces of data, all in a specific path, every PathSpace or Data that is contained has a name. This forms a tree structure similar to a file system on a computer where folders are PathSpaces and files are pieces of data. The Data class can be seen as a FIFO queue of typed data and could for example contain a sequence such as (int, float, int, A, std::string, lambda) where A is a user defined serializable class and lambda is a method that can generate a value when read.

## Polymorphism
A PathSpace class can act as a parent class to a user supplied child class which can in turn be inserted into a PathSpace of any type. This way sub-paths of the PathSpace can behave differently than others. For example one part of a tree could mount a folder from the local file system, another could be a scene graph of a game engine that deals with managing GPU state. Another path could be an abstraction over the operating system similar to the Unix philosophy of "everything is a file", enabling a platform independent way of for example reading the mouse position or creating a window to draw 3d models into.
PathSpace will by default create child path spaces of the same type as the parent. They are implemented via std::unique_ptr<PathSpace> which means they can override the base class methods with their own implementation. 

## Operations
The operations in the base language are insert/read/grab, they are implemented as member functions of the PathSpace class.
* **Insert**: 
	* Insert data or a PathSpace to one or more paths. If the path does not exist it will be created.
	* The given path can be a concrete path in which case at most one object will be inserted or a glob expression path which could potentially insert multiple values.
	* Supports batch operations by inserting an initialiser list
	* Takes an optional InsertOptions which has the following properties:
		* Optional capability object to determine if the PathSpace is allowed to insert at the specified path.
		* Optional Time To Live (TTL) that specifies how long the data will exist before being deleted. By default they live forever.
		* Optional Execution object that describes how to execute the data (if the data is a lambda or function):
			* Execute immediately or when the user requests the data via read/grab.
			* If the data should be cached and updated every n milliseconds.
			* If the execution should be from the globally registered executions database via a supplied ID.
			* How many times the function should be executed.
			* If the execution object should be turned into it’s return value or if the return value should be returned leaving the lambda in place on issuing of a read operation to the execution.
		* Optional Block object specifying what to do if a space in the insert path is locked:
			* Wait forever
			* Wait a specified amount of milliseconds
			* Return an error
	* The data inserted can be:
		* Executions with signature T(ConcretePath const &path, PathSpace &space) :
			* Lambda
			* Function pointer
			* std::function
			* Preregistered global executions of the above types tagged with an ID, this method supports serialisation/deserialisation over the network. This is specified in the InsertOptions and renders the input data meaningless.
		* Data
			* Fundamental types
			* Standard library containers if serialisable
			* User created structures/classes as long as they are serialisable
	* Returns a InsertReturn structure with the following information:
		* How many items were inserted.
		* What errors occurred during insertion.
	* Syntax:
		* InsertReturn PathSpace::insert<T>(GlobPath const &path, T const &value, optional<InsertOptions> const &options={})
	* Example:
		* InsertReturn val = space.insert("/test", 5); // insert 5 to “/test”
		* InsertReturn val = space.insert(“/*/abc”, 34); // insert 34 to, for example, “/test/abc” and “/second/abc”.
		* InsertReturn val = space.insert(“/a”, [](ConcretePath const &path, PathSpace &space){return 54;});
		* InsertReturn val = space.insert(“/a”, ExecutionOptions{.storedFunction=”a”, .numberOfExecutions=10});
* **Read**:
	* Returns a copy of the value at the supplied path or Error if it could not be found, if for example the path did not exist or the front value had the wrong type.
	* Takes an optional ReadOptions which has the following properties:
		* Optional Block object specifying what to do if the data does not exist or paths to the data do not exist:
			* Wait forever if data does not exist
			* Wait a specified amount of milliseconds if data does not exist
			* Wait forever if space does not exist
			* Wait a specified amount of milliseconds if space does not exist
			* Return an error
		* Optional capability object to determine if the PathSpace is allowed to read at the specified path.
		* If the output should be a JSON view of the path, if so T must be std::string
	* Takes a ConcretePath, does not support GlobPaths.
	* Syntax:
		* std::expected<T, Error> PathSpace::read<T>(ConcretePath const &path, optional<ReadOptions> const &options={})
	* Example:
		* std::expected<T, Error> val = space.read<int>(“/test”); // returns the value at the path “/test” if it exists, else an Error
		* std::expected<T, Error> val = space.read<int>(“/test”, ReadOptions{.block=true, .blockTime=30s}); // returns the value at the path “/test” if it exists, else wait 30s, if it has not been found by then return an error.
		* std::expected<T, Error> val = space.read<int>(“/test”, ReadOptions{.block=true, Capabilities{“/**”}); // Has a capacity that allows reading any data, block forever waiting for the data, or until the PathSpace destructor is called
		* std::expected<PathSpace::json, Error> val = space.read<PathSpace::json>(“/test”); // Reads “/test” and it’s children back as a json object, if there are user objects that do not support conversion to json they become empty objects in the json.
		* std::expected<std::shared_ptr<PathSpace>, Error> val = space.read<std::shared_ptr<PathSpace>>(“/test”); // returns a shared_ptr to a PathSpace at path “/test”
* **Grab**: 
	* Same as read but pops the data instead of just returning a copy.
	* Syntax:
		* std::expected<T, Error> PathSpace::grab<T>(ConcretePath, Block, optional<GrabOptions> const &options={})


## Blocking 
It’s possible to send a blocking object to insert/read/grab instructing it to wait a certain amount of time for data to arrive if it is currently empty or non-existent.

## Capability
The capability functionality enables PathSpaces to have fine grained control over inserting/modifying or reading data in a PathSpace. If for example executions/lambdas can be inserted or if data at a specific path can be read. The capabilities are glob path based, controlling access over what parts of the tree can be accessed for what. A capability object can have control over multiple glob paths inside of it. 
The following capabilities exist:
* Read
* Grab
* Mutate
* Execute

## Data Storage
A normal PathSpace will store data by serialising it to a std::vector<std::byte>. That vector can contain data of different types and a separate vector storing std::type_id pointers together with how many objects or that type are in a row will be used to determine what parts of the data vector has what type. Std::function objects will be stored in their own vector as well since they can not be serialised. Insert will append serialised data to this vector. Grab will not necessarily erase from the front of the vector since this would be too costly, a pointer to the front element will instead be stored and its position changed forward when a grab is issued. At first the serialisation will be done via the alpaca library but when a compiler supporting the C++26 serialisation functionality it will be rewritten to use that instead.

## Glob Expressions
Paths can be glob expressions for the insert operation, the data will be copied and inserted in any matching path. A glob expression in insert cannot create paths since they have nothing to match against.

## JSON Serialisation
Serialisation will be supported out from the PathSpace into JSON in order to enable to do introspection and visualisation of the internal data of the path space or for export of performance data. For example to show a filesystem view of the tree in a GUI where individual data can be viewed. Will also support deserialisation for loading old state. Binary serialisation can also be done to for example create a state of the path space that can be serialised later for example for typical save/load functionality. This is achieved by specifying a json data type for read or grab, example: space.read<std::string>(“/test”, ReadOptions{.toJSON=true})

## Multi-threading
All the PathSpace operations will be thread safe. The threading will be done with a thread pool. Much of the heavy lifting insuring thread safety will be done via the phmap::parallel_flat_hash_map library. The possible executabler objects that can be inserted into a PathSpace is:
* Function pointers
* std::function
* Functor object
All of these can take an optional ExecutionOptions parameter when launched. That struct will give information on:
* What PathSpace the executions is from.
* What path the execution was inserted into.
* If the PathSpace is still alive or being destroyed.
This functionality will be added to a TaskPool class which will be available either as a non-copyable normal object or as a Singleton which was created before main. It will create threads as needed and will be pushing tasks on to a task queue that the threads can queue on for recieving more work.

## Memory
In order to avoid memory allocation calls to the operating system the PathSpace could have a pre-allocated pool of memory from the start. Perhaps some PathSpace tuple data could be marked as non essential somehow, if an out of memory situation occurs such data could be erased or flushed to disk. The alpha version of PathSpace will not contain this feature and will allocate all memory via std standard allocators.

## Unit Testing
Unit testing will be done by using the C++ doctest library. A test driven development method will be used.

## Tools
CMake
Clang
GCC
VSCode
Ninja
Alpaca
PhMap
doctest

## Metadata
Every PathSpace will have some metadata such as last time read, last time modified, how many times modified (same as version). Testing will be done by using a special CMake project that uses one .cpp file per operation to test.

## Logging
A log of every action can be produced. This is mostly for aiding in debugging.

## Documentation
Documentation with examples and descriptions of the API will be provided on the GitHub page for the project.

## Exception Handling
PathSpaces will not throw exceptions, all errors will be handled via the return type of the operations.

## Load Balancing
TBD

## Distributed Networking
### Transaction Support
TBD
### Multi-Version Consistency Control
TBD
### Conflict-Free Replicated Data Types
TBD
### Invariant-based Reasoning
TBD

## Fault Tolerance
TBD

## Licensing
PathSpace will be under an open source license, likely GPL-3. I don’t mind if commercial entities use it as long as they contribute back code. In the unlikely event of commercial interest they can license it under a different license for a cost.

## Back-Pressure Handling
TBD

## Lazy Evaluation
TBD

## Hot vs Cold Observables
TBD

## GUI-based Monitoring Tool
TBD

## Live Objects
TBD

## Trellis
Lazy executions can be chained together via blocking operations waiting for new data, when one execution updates a value another listener to that value can execute. This creates a trellis like structure as described in the book mirror worlds book where values are fed from bottom processes and filtered up into higher order processes.

## Actor Based Programming
An actor system could be implemented with PathSpace by having executions within the PathSpace as actors communicating via messages sent to mailboxes at predefined paths, the mailboxes could simply be std::string objects stored at those paths.

## Default Paths
TBD

## Benchmarks
Linda like tuple spaces have traditionally been seen as slow. To some degree this is due to the pattern matching required to extract data from a tuple. In PathSpace the slowest part will be traversing the path hierarchy. Potentially this could be sped up if the user could manually lock an internal PathSpace from being erased and returning a pointer to it so that the path would not need to be traversed in order to perform operations on it and then unlocking it when done, perhaps via RAII. But this is a bit heavy handed, undecided.

### Version Migration Utilities
TBD

### Metrics Collection
Will be possible to collect metrics on the data for the paths, how often they are accessed etc.

### Data Integrity
Especially for the networking part some checking for data consistency will be needed.

### Reactive programming
The extendability of a PathSpace can be used to create a reactive data flow, for example by creating a child PathSpace that takes n other PathSpace paths as input and combines their values by grabbing from them or a PathSpace that performs a transformation on any data from n other paths. Each tuple can be seen as a data stream as long as they are alive, when they run out of items the stream dies. One example could be a lambda within the space that waits for the left mouse button to be pressed via space.read<int>(“/system/mouse/button_down”, ReadOptions{.block=true}).

### Distributed Data
Another specialisation is to create a PathSpace child that can replicate a space over several computers over the network. Duplicating data where needed. May use Asia for networking. Like a live object.

## Example Use Cases
* Scene Graph - Objects to be displayed by a 3d renderer. Mainly takes care of seamlessly interacting with the renderer to upload and display objects in the graph. Could also support data oriented design by having composable objects that store their properties in a list instead of objects in a list.
* Vulcan Render - A renderer that integrates with the GPU via Vulcan. Makes it easy to upload and use meshes and shaders.
* ISO Surface Generator - Just in time generation of geometry to display, taking LOD into account. It can have an internal path like …/generate/x_y_y that can be read to start the generation.
* Operating System - The capabilities functionality can be used to implement a user system and file system similar to Unix. When the initial PathSpace has been filled a process for the root user with full capability can be started by launching a lambda that executes a script. This is similar to how on login a bash session is started with a default script. This process can in turn launch other processes similar to systemd or cron.
* Database front end - Interpreter to a proper database library adding or changing data within it. For example mariadb or postgressql.
* GraphQL server to interact with html/javascript

### Temporary Data Path
Will not write out any data for state saving and if a user logs out all their created data will be deleted. Will usually be at /tmp, with user dirs as /tmp/username.

### Scripting
There will be support for manipulating a PathSpace through lua. Issuing an operator can then be done without having to reference a space object, like so: insert(“/a”, 5). But separate space objects could be created as variables in the script and could be used by the normal space_name.insert(… usage.

### Command line
Using a live script interpreter a command line can be written.

### Compression
Compresses the input data seamlessly behind the scene on insert and decompresses it on read/grab.

### Out-of-core
Similar to compression but can store the data on the hard drive instead of in RAM.
