// https://github.com/Shopify/react-native-skia/issues/1529
import React, { useState } from "react";
import { Button, View } from "react-native";
import { makeMutable, runOnUI } from "react-native-reanimated";

function Component() {
  const a = makeMutable(12345);
  const sv = makeMutable(0);
  const b = makeMutable(67890);
  // console.log(sv.value);
  // runOnUI(() => {
  //   console.log(sv.value);
  // })();
  // console.log(sv.value);
  return (
    <View style={{ flex: 1, marginTop: 50 }}>
      <Button title="run 1" onPress={() => { sv.value = new Float32Array([1,2,3])}} />
      <Button title="run 2" onPress={() => { console.log(sv.value)}} />
      <Button title="run 3" onPress={() => {}} />
    </View>
  )
}

export const App = () => {
  const [toggle, setToggle] = useState(false);
  const a = makeMutable(0);
  return (
    <View style={{ flex: 1, marginTop: 50 }}>
      <Button 
      title="Crash my app please" 
      onPress={() => {
        setToggle(!toggle);
      }} />
      {toggle && <Component />}
    </View>
  );
};

export default App;
