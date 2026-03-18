CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra
# Statically link all libraries for portability to test machines
LDFLAGS = -static -lgtest -lgtest_main -lpthread

TARGET = ptshare_test
SRC = ptshare_test.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run
