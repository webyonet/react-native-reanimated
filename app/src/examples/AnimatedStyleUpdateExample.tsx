import Animated, {
  useSharedValue,
  withTiming,
  useAnimatedStyle,
  Easing,
  runOnUI,
  runOnJS,
  createWorkletRuntime,
} from 'react-native-reanimated';
import { View, Button, StyleSheet } from 'react-native';
import React from 'react';
import { makeShareableCloneRecursive } from 'react-native-reanimated/src/reanimated2/shareables';
import { runOnRuntimeSync } from '../../../src/reanimated2/runtimes';

export default function AnimatedStyleUpdateExample(): React.ReactElement {
  const randomWidth = useSharedValue(10);

  const config = {
    duration: 500,
    easing: Easing.bezierFn(0.5, 0.01, 0, 1),
  };

  const style = useAnimatedStyle(() => {
    return {
      width: withTiming(randomWidth.value, config),
    };
  });

  const handlePress1 = () => {
    const func = () => console.log('xd1');
    runOnUI(() => {
      'worklet';
      runOnJS(func)();
    })();
  };

  const handlePress2 = () => {
    global._scheduleOnJS(
      makeShareableCloneRecursive(console.log),
      makeShareableCloneRecursive(['xd2'])
    );
  };

  const handlePress3 = () => {
    const runtime = createWorkletRuntime('foo');
    console.log(runtime);
    console.log(`${runtime}`);
    console.log(String(runtime));
  };

  const handlePress4 = () => {
    const runtime = createWorkletRuntime('foo');
    runOnRuntimeSync(runtime, () => {
      'worklet';
      console.log('Hello from runtime');
    });
  };

  const handlePress5 = () => {
    const runtime = createWorkletRuntime('foo');
    function bar() {
      'worklet';
      throw new Error('Hello world!');
    }
    function foo() {
      'worklet';
      bar();
    }
    runOnRuntimeSync(runtime, () => {
      'worklet';
      foo();
    });
    // TODO: fix missing stack trace
  };

  return (
    <View style={styles.container}>
      <Animated.View style={[styles.box, style]} />
      <Button
        title="toggle"
        onPress={() => {
          randomWidth.value = Math.random() * 350;
        }}
      />
      <Button title="runOnUI / runOnJS" onPress={handlePress1} />
      <Button title="_scheduleOnJS" onPress={handlePress2} />
      <Button title="createWorkletRuntime" onPress={handlePress3} />
      <Button title="runOnRuntimeSync" onPress={handlePress4} />
      <Button title="throw new Error" onPress={handlePress5} />
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    flexDirection: 'column',
  },
  box: {
    width: 100,
    height: 80,
    backgroundColor: 'black',
    margin: 30,
  },
});
