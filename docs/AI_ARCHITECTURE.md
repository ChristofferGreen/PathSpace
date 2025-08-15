# PathSpace
![PathSpace](images/PathSpace.jpeg)

## Introduction
PathSpace is a coordination language that enables insertion and extractions from paths in a thread safe datastructure. The data structure supports views of the paths similar to Plan 9. The data attached to the paths are more like a JSON datastructure than files though. The data supported is standard C++ data types and data structures from the standard library, user created structs/classes as well as function pointers, std::function or function objects for storing executions that generate values to be inserted at a path.

## Path System

### Path Types

#### Concrete Paths
Concrete paths represent exact locations in the PathSpace hierarchy. They follow these rules:
- Must start with a forward slash (/)
- Components are separated by forward slashes
- Cannot contain relative path components (. or ..)
- Names cannot start with dots
- Components cannot be empty (// is normalized to /)

Example:
```cpp
"/data/sensors/temperature"
"/system/status/current"
```

#### Glob Paths
Glob paths extend concrete paths with pattern matching capabilities. They support:
- `*` - Matches any sequence of characters within a component
- `**` - Matches zero or more components (super-matcher)
- `?` - Matches any single character
- `[abc]` - Matches any character in the set
- `[a-z]` - Matches any character in the range
- `\` - Escapes special characters

Example patterns:
```cpp
"/data/*/temperature"    // Matches any sensor's temperature
"/data/**/status"       // Matches status at any depth
"/sensor-[0-9]/*"       // Matches numbered sensors
"/data/temp-?"          // Matches single character variations
```

### Path Implementation

#### Core Classes
```cpp
// Base path template
template <typename T>
struct Path {
    T path;  // string or string_view storage
    bool isValid() const;
    T const& getPath() const;
};

// Concrete path types
using ConcretePathString = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

// Glob path types
using GlobPathString = GlobPath<std::string>;
using GlobPathStringView = GlobPath<std::string_view>;
```

#### Path Component Iteration
The system provides iterators to traverse path components:
```cpp
template <typename T>
struct ConcreteIterator {
    ConcreteNameStringView operator*() const;  // Current component
    bool isAtStart() const;                    // Check if at first component
    std::string_view fullPath() const;         // Full path string
};

template <typename T>
struct GlobIterator {
    GlobName operator*() const;  // Current component with pattern matching
    bool isAtStart() const;      // Check if at first component
    std::string_view fullPath() const;  // Full path string
};
```

#### Pattern Matching
Pattern matching can handle various glob patterns:

```cpp
struct GlobName {
    // Returns match status
    auto match(const std::string_view& str) const 
        -> bool /*match*/;
    
    bool isConcrete() const;  // Check if contains patterns
    bool isGlob() const;      // Check for glob characters
};
```

Pattern matching behaviors:
- `*` matches within component boundaries
- Character classes support ranges and sets
- Escaped characters are handled literally
- Empty components are normalized

### Path Usage Examples

#### Basic Path Operations
```cpp
PathSpace space;

// Insert at concrete path
space.insert("/sensors/temp1/value", 23.5);

// Insert using pattern
space.insert("/sensors/*/value", 0.0);  // Reset all sensor values

// Read with exact path
auto value = space.read<float>("/sensors/temp1/value");

// Read with pattern
auto values = space.read<float>("/sensors/*/value");
```

#### Pattern Matching Examples
```cpp
// Match any sensor
"/sensors/*"

// Match sensors with numeric IDs
"/sensors/[0-9]*"

// Match any depth under sensors
"/sensors/**/value"

// Match specific character variations
"/sensor-?"

// Match range of sensors
"/sensors/[1-5]"
```

### Path Performance Considerations

#### Path Storage
- Uses `string_view` for read-only operations to avoid copies
- Maintains string ownership where needed for modifications
- Caches path components for efficient iteration

#### Pattern Matching Optimization
- Early termination for non-matching patterns
- Efficient character class evaluation
- Optimized super-matcher (`**`) handling
- Component-wise matching to minimize string operations

#### Memory Efficiency
- Shared path component storage
- Lazy evaluation of pattern matches
- Efficient string views for immutable paths

### Path Thread Safety
The path system provides thread-safe operations through:
- Immutable path objects
- Synchronized pattern matching
- Thread-safe path resolution
- Atomic path operations

Additional note:
- Task completion notifications are lifetime-safe via a NotificationSink token. Each `PathSpaceBase` owns a `shared_ptr<NotificationSink>` that forwards to its `notify(path)`; `Task` objects capture a `weak_ptr<NotificationSink>`. During shutdown, the `PathSpace` resets the `shared_ptr` so late notifications are dropped cleanly without dereferencing stale pointers.

### Path Error Handling
Comprehensive error handling for path operations:
- Path validation errors
- Pattern syntax errors
- Resolution failures
- Type mismatches
- Timeout conditions

## Internal Data
PathSpace stores data in a unified Node trie. Each Node contains:
- children: a concurrent hash map keyed by the next path component
- data: an optional NodeData payload that holds serialized bytes and/or queued Task objects
- nested: an optional nested PathSpaceBase anchored at that node

Inserting more data at a path appends to the node’s NodeData sequence. Reading returns a copy of the front element; extracting pops the front element. Nodes that become empty after extraction are erased from their parent when safe.

Concurrency notes:
- children uses a sharded concurrent map for scalability
- payload accesses (data/nested) are guarded by a per-node mutex to ensure safe updates under contention

Additionally, nodes may hold `Task` objects representing deferred computations alongside serialized bytes. `NodeData` stores tasks (`std::shared_ptr<Task>`) and, when tasks complete, notifications are delivered via a `NotificationSink` interface: tasks hold a `std::weak_ptr<NotificationSink>` and the owning `PathSpaceBase` provides the `shared_ptr` token and forwards to `notify(path)`. This removes the need for a global registry and prevents use-after-free races during teardown (the space resets its token during shutdown, causing late notifications to be dropped safely).

## Syntax Example
```
PathSpace space;
space.insert("/collection/numbers", 5);
space.insert("/collection/numbers", 3.5f);
assert(space.take<int>("/collection/numbers").value() == 5);
assert(space.read<float>("/collection/numbers").value() == 3.5);

space.insert("/collection/executions", [](){return 7;});
assert(space.take<int>("/collection/numbers", Block{}).value() == 7);
```

## Polymorphism
The internal spaces inside a path are implemented witha  Leaf class, it's possible to inherit from that class and insert that child class in order to change the
behaviour of parts of the path structure of a PathSpace instance. This can be used to have different behaviour for different sub spaces within a PathSpace. By default PathSpace
will create a Leaf of the same type as the parent when creating new ones (which happens during insert of data).

## Path Globbing
Paths given to insert can be a glob expression, if the expression matches the names of subspaces then the data will be inserted to all the matching subspaces.
````
PathSpace space;
space.insert("/collection/numbers", 5);
space.insert("/collection/numbers_more", 4);
space.insert("/collection/other_things", 3);
space.insert("/collection/numbers*", 2);
assert(space.take<int>("/collection/other_things").value() == 2);
assert(!space.take<int>("/collection/other_things").has_value());
assert(space.take<int>("/collection/numbers_more").value() == 4);
assert(space.take<int>("/collection/numbers_more").value() == 2);
```

## Blocking
It's possible to send a blocking object to insert/read/extract instructing it to wait a certain amount of time for data to arrive if it is currently empty or non-existent.

## Operations
The operations in the base language are insert/read/extract, they are implemented as member functions of the PathSpace class.
* **Insert**: 
	* Insert data or a Leaf to one or more paths. If the path does not exist it will be created.
	* The given path can be a concrete path in which case at most one object will be inserted or a glob expression path which could potentially insert multiple values.
	* Supports batch operations by inserting an initialiser list
	* Takes an optional In which has the following properties:
		* Optional Execution object that describes how to execute the data (if the data is a lambda or function):
			* Execute immediately or when the user requests the data via read/extract.
			* If the data should be cached and updated every n milliseconds.
				* How many times the function should be executed.
			* If the value to be stored is executed in a lazy fashion or right away.
		* Optional Block object specifying what to do if the value does not (yet) exist, enables waiting forever or a set amount of time.
	* The data inserted can be:
		* Executions with signature T() or T(ConcretePath const &path, PathSpace &space) :
			* Lambda
			* Function pointer
			* std::function
			* Preregistered executions for serialisation/deserialisation over the network.
		* Data
			* Fundamental types
			* Standard library containers if serialisable
			* User created structures/classes as long as they are serialisable
	* Returns an InsertReturn structure with the following information:
		* How many items/Tasks were inserted.
		* What errors occurred during insertion.
	* Syntax:
		* InsertReturn PathSpace::put<T>(GlobPath const &path, T const &value, optional<In> const &options={})
* **Read**:
	* Returns a copy of the front value at the supplied path or Error if it could not be found, if for example the path did not exist or the front value had the wrong type.
	* Takes an optional ReadOptions which has the following properties:
		* Optional Block object specifying what to do if the data does not exist or paths to the data do not exist:
			* Wait forever if data/space does not exist
			* Wait a specified amount of milliseconds if data/space does not exist
			* Return an error
	* Takes a ConcretePath, does not support GlobPaths. Perhaps will implement a readMultiple later that returns a vector<T>
	* Syntax:
		* std::expected<T, Error> PathSpace::read<T>(ConcretePath const &path, optional<ReadOptions> const &options={})
* **Extract**: 
	* Same as read but pops the front data instead of just returning a copy.
	* Syntax:
		* std::expected<T, Error> PathSpace::take<T>(ConcretePath, Block, optional<Out> const &options={})

## Data Storage
A normal PathSpace will store data by serialising it to a std::vector<std::byte>. That vector can contain data of different types and a separate vector storing std::type_id pointers
together with how many objects or that type are in a row will be used to determine what parts of the data vector has what type. std::function objects will be stored in their own vector
as well since they can not be serialised. Insert will append serialised data to this vector. Extract will not necessarily erase from the front of the vector since this would be too costly,
a pointer to the front element will instead be stored and its position changed forward when a extract is issued. At first the serialisation will be done via the alpaca library but when a
compiler supporting the C++26 serialisation functionality it will be rewritten to use that instead.

## Unit Testing
Unit testing will be done by using the C++ doctest library. A test driven development method will be used.

## Logging
A log of every action can be produced. This is mostly for aiding in debugging.

## Exception Handling
PathSpaces will not throw exceptions, all errors will be handled via the return type of the operations.

## Licensing
PathSpace will be under an open source license, likely GPL-3.

## Trellis
Lazy executions can be chained together via blocking operations waiting for new data, when one execution updates a value another listener to that value can execute.
This creates a trellis like structure as described in the book mirror worlds book where values are fed from bottom processes and filtered up into higher order processes.

## Actor Based Programming
An actor system could be implemented with PathSpace by having executions within the PathSpace as actors communicating via messages sent to mailboxes at predefined paths, the mailboxes
could simply be std::string objects stored at those paths.

## Bottlenecks
Linda like tuple spaces have traditionally been seen as slow. To some degree this is due to the pattern matching required to extract data from a tuple. 
### Path Caching
In PathSpace the slowest part will be traversing the path hierarchy. Potentially this could be sped up if we provide a cache of the most recent lookups of paths for insert/read/extract.
Could be done as a hashmap from a ConcretePath to a PathSpace*. Would need to clear the cache on extract, or other PathSpace removals.

### Reactive programming
The extendability of a PathSpace can be used to create a reactive data flow, for example by creating a child PathSpace that takes n other PathSpace paths as input and combines their
values by extractbing from them or a PathSpace that performs a transformation on any data from n other paths. Each tuple can be seen as a data stream as long as they are alive, when
they run out of items the stream dies. One example could be a lambda within the space that waits for the left mouse button to be pressed via space.read<int>("/system/mouse/button_down",
ReadOptions{.block_=true}).

## Example Use Cases
* Scene Graph - Objects to be displayed by a 3d renderer. Mainly takes care of seamlessly interacting with the renderer to upload and display objects in the graph. Could also support data oriented design by having composable objects that store their properties in a list instead of objects in a list.
* Vulcan Render - A renderer that integrates with the GPU via Vulcan. Makes it easy to upload and use meshes and shaders.
* ISO Surface Generator - Just in time generation of geometry to display, taking LOD into account. It can have an internal path like …/generate/x_y_y that can be read to start the generation.
* Operating System - The capabilities functionality can be used to implement a user system and file system similar to Unix. When the initial PathSpace has been filled a process for the root user with full capability can be started by launching a lambda that executes a script. This is similar to how on login a bash session is started with a default script. This process can in turn launch other processes similar to systemd or cron.
* Database front end - Interpreter to a proper database library adding or changing data within it. For example mariadb or postgressql.
* GraphQL server to interact with html/javascript

## Not Yet Implemented Features

### Temporary Data Path
Will not write out any data for state saving and if a user logs out all their created data will be deleted. Will usually be at /tmp, with user dirs as /tmp/username.

### Scripting
There will be support for manipulating a PathSpace through lua. Issuing an operator can then be done without having to reference a space object, like so: insert("/a", 5).
But separate space objects could be created as variables in the script and could be used by the normal space_name.insert(… usage.

### Command line
Using a live script interpreter a command line can be written.

### Compression
Compresses the input data seamlessly behind the scene on insert and decompresses it on read/extract.

### Out-of-core
Similar to compression but can store the data on the hard drive instead of in RAM.

### Distributed Data
Another specialisation is to create a PathSpace child that can replicate a space over several computers over the network. Duplicating data where needed. May use Asia for networking.
Like a live object.

## Memory (not yet implemented)
In order to avoid memory allocation calls to the operating system the PathSpace could have a pre-allocated pool of memory from the start. Perhaps some PathSpace tuple data could be marked
as non essential somehow, if an out of memory situation occurs such data could be erased or flushed to disk. The alpha version of PathSpace will not contain this feature and will allocate
all memory via std standard allocators.

## Metadata (not yet implemented)
Every PathSpace will have some metadata such as last time read, last time modified, how many times modified (same as version). Testing will be done by using a special CMake project that uses one .cpp file per operation to test.

## Documentation (not yet implemented)
Documentation with examples and descriptions of the API will be provided on the GitHub page for the project.

### Metrics Collection (not yet implemented)
Will be possible to collect metrics on the data for the paths, how often they are accessed etc.

### Data Integrity (not yet implemented)
Especially for the networking part some checking for data consistency will be needed.

## Distributed Networking
### Transaction Support
TBD
### Multi-Version Consistency Control
TBD
### Conflict-Free Replicated Data Types
TBD
### Invariant-based Reasoning
TBD

### Version Migration Utilities
TBD

## Back-Pressure Handling
TBD

## Default Paths
TBD

## Live Objects
TBD

## Fault Tolerance
TBD

## Views
TBD

## Operating System
TBD